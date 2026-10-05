#!/usr/bin/env python3
"""Test source provenance and local-install cleanup without running Homebrew.

Uses temporary Git repositories, real source archives, and a mock brew command.
Run with: python3 chimera/packaging/homebrew/test-packaging.py
"""
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest


HERE = Path(__file__).resolve().parent
BREW = r'''
import json
import os
from pathlib import Path
import sys

args = sys.argv[1:]
with open(os.environ["MOCK_BREW_CALLS"], "a") as output:
    output.write(json.dumps(args) + "\n")
state = Path(os.environ["MOCK_LINK_STATE"])
if args == ["--repository"]:
    print(os.environ["MOCK_BREW_REPO"])
elif args == ["command", "trust"]:
    sys.exit(1)
elif args[:2] == ["info", "--json=v2"]:
    print(json.dumps({"formulae": [{"linked_keg": state.read_text() or None,
                                   "installed": [] if os.environ.get("MOCK_ABSENT") else [{"version": "1.2.3"}]}]}))
elif args[0] == "reinstall":
    if not os.environ.get("MOCK_ABSENT"):
        state.write_text("1.2.3")
    sys.exit(int(os.environ.get("MOCK_REINSTALL_STATUS", "0")))
elif args[0] == "install" and "--skip-link" in args:
    pass
elif args[0] == "test":
    sys.exit(int(os.environ.get("MOCK_TEST_STATUS", "0")))
elif args[0] == "unlink":
    status = int(os.environ.get("MOCK_UNLINK_STATUS", "0"))
    if not status:
        state.write_text("")
    sys.exit(status)
else:
    sys.exit("unexpected brew invocation: " + repr(args))
'''


class PackagingFixture(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="chimeradb-packaging-test-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.repo = self.root / "repo"
        self.homebrew = self.repo / "chimera/packaging/homebrew"
        self.homebrew.mkdir(parents=True)
        for name in ("local-test.sh", "render-formula.py", "formula.rb.in"):
            shutil.copyfile(HERE / name, self.homebrew / name)
        shutil.copyfile(HERE.parent / "source.sh", self.homebrew.parent / "source.sh")
        self.version = self.repo / "chimera/VERSION"
        self.version.write_text("1.2.3\n")
        (self.repo / "README.md").write_text("Fixture repository\n")
        self.git("init", "--quiet", "--template=")

    def git(self, *args):
        return subprocess.check_output(["git", "-C", str(self.repo), *args], text=True)


class LocalTestCleanupTest(PackagingFixture):
    def setUp(self):
        super().setUp()
        self.calls_file = self.root / "brew.jsonl"
        self.link_state = self.root / "linked-keg"
        self.link_state.write_text("")
        brew_repo = self.root / "brew"
        (brew_repo / "Library/Taps/chimera-local/homebrew-release-validation/.git").mkdir(parents=True)
        mock_bin = self.root / "bin"
        mock_bin.mkdir()
        for name, script in (("brew", BREW), ("uname", 'print("Darwin")\n')):
            command = mock_bin / name
            command.write_text("#!" + sys.executable + "\n" + script)
            command.chmod(0o755)
        self.env = dict(os.environ, PATH=str(mock_bin) + os.pathsep + os.environ["PATH"],
                        MOCK_BREW_CALLS=str(self.calls_file), MOCK_LINK_STATE=str(self.link_state),
                        MOCK_BREW_REPO=str(brew_repo))

    def run_local_test(self, *, reinstall=True, server="11.8", **settings):
        args = ["bash", str(self.homebrew / "local-test.sh"), "--server", server,
                "--work-dir", str(self.root / "work")]
        if reinstall:
            args.append("--reinstall")
        return subprocess.run(args, env=dict(self.env, **settings), capture_output=True,
                              text=True, timeout=20)

    def calls(self, command):
        return [args for args in map(json.loads, self.calls_file.read_text().splitlines())
                if args[0] == command]

    def assert_unlinked(self):
        self.assertEqual(self.link_state.read_text(), "")
        self.assertEqual(len(self.calls("unlink")), 1)

    def test_unlinked_reinstall_success_restores_links_and_forces_formula_test(self):
        result = self.run_local_test(server="10.11")
        self.assertEqual(result.returncode, 0, result.stderr)
        formula = "chimera-local/release-validation/chimeradb@10.11"
        self.assertEqual(self.calls("reinstall"), [["reinstall", "--build-from-source", formula]])
        self.assertEqual(self.calls("test"), [["test", "--force", formula]])
        self.assertEqual(self.calls("unlink"), [["unlink", formula]])
        self.assert_unlinked()

    def test_build_failure_keeps_its_exit_status_and_restores_links(self):
        result = self.run_local_test(MOCK_REINSTALL_STATUS="23")
        self.assertEqual(result.returncode, 23, result.stderr)
        self.assertEqual(self.calls("test"), [])
        self.assert_unlinked()

    def test_formula_test_failure_keeps_its_exit_status_and_restores_links(self):
        result = self.run_local_test(MOCK_TEST_STATUS="47")
        self.assertEqual(result.returncode, 47, result.stderr)
        self.assert_unlinked()

    def test_cleanup_failure_turns_success_into_failure(self):
        result = self.run_local_test(MOCK_UNLINK_STATUS="31")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("could not restore unlinked state", result.stderr)
        self.assertEqual(self.link_state.read_text(), "1.2.3")
        self.assertEqual(len(self.calls("unlink")), 1)

    def test_cleanup_failure_does_not_mask_formula_test_failure(self):
        result = self.run_local_test(MOCK_TEST_STATUS="47", MOCK_UNLINK_STATUS="31")
        self.assertEqual(result.returncode, 47, result.stderr)
        self.assertIn("could not restore unlinked state", result.stderr)
        self.assertEqual(len(self.calls("unlink")), 1)

    def test_initially_linked_formula_stays_linked_on_success_and_failure(self):
        for status in ("0", "47"):
            with self.subTest(test_status=status):
                self.link_state.write_text("1.2.3")
                result = self.run_local_test(MOCK_TEST_STATUS=status)
                self.assertEqual(result.returncode, int(status), result.stderr)
                self.assertEqual(self.link_state.read_text(), "1.2.3")
                self.assertEqual(self.calls("unlink"), [])

    def test_absent_formula_reinstall_failure_is_not_masked_by_unlink_failure(self):
        result = self.run_local_test(MOCK_ABSENT="1", MOCK_REINSTALL_STATUS="14", MOCK_UNLINK_STATUS="31")
        self.assertEqual(result.returncode, 14, result.stderr)
        self.assertEqual(self.link_state.read_text(), "")
        self.assertEqual(self.calls("test"), [])
        self.assertEqual(len(self.calls("unlink")), 1)

    def test_first_install_uses_skip_link_and_never_restores_unqueried_state(self):
        result = self.run_local_test(reinstall=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        formula = "chimera-local/release-validation/chimeradb"
        self.assertEqual(self.calls("install"), [["install", "--build-from-source", "--skip-link", formula]])
        self.assertEqual(self.calls("test"), [["test", "--force", formula]])
        self.assertEqual(self.calls("info"), [])
        self.assertEqual(self.calls("unlink"), [])
        self.assertEqual(self.link_state.read_text(), "")


class FormulaArchiveTest(PackagingFixture):
    def render(self, archive, *, checksum=None):
        self.output = self.root / "Formula"
        return subprocess.run([sys.executable, str(self.homebrew / "render-formula.py"),
                               "--source-archive", str(archive),
                               "--url", "https://example.invalid/releases/" + archive.name,
                               "--sha256", checksum or hashlib.sha256(archive.read_bytes()).hexdigest(),
                               "--output", str(self.output)],
                              capture_output=True, text=True, timeout=10)

    def archive(self, entries):
        path = self.root / "source.tar.gz"
        with tarfile.open(path, "w:gz") as archive:
            for name, data, kind in entries:
                member = tarfile.TarInfo(name)
                member.type = kind
                if kind == tarfile.REGTYPE:
                    member.size = len(data)
                    archive.addfile(member, io.BytesIO(data))
                else:
                    member.linkname = "/not-read-by-test"
                    archive.addfile(member)
        return path

    def assert_rejected(self, result, message):
        self.assertNotEqual(result.returncode, 0, result.stderr)
        self.assertIn(message, result.stderr)
        self.assertFalse(self.output.exists())

    def test_exported_older_ref_supplies_version_despite_newer_checkout_and_dirty_version(self):
        self.git("add", ".")
        self.git("-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
                 "-c", "core.hooksPath=/dev/null", "commit", "--quiet", "-m", "Older release")
        older = self.git("rev-parse", "HEAD").strip()
        self.version.write_text("2.0.0\n")
        self.git("add", "chimera/VERSION")
        self.git("-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
                 "-c", "core.hooksPath=/dev/null", "commit", "--quiet", "-m", "Newer release")
        self.version.write_text("3.0.0\n")
        output = self.root / "exported"
        subprocess.run(["bash", str(self.homebrew.parent / "source.sh"), "--ref", older,
                        "--output", str(output)], check=True, capture_output=True, text=True, timeout=10)
        archive = output / "chimeradb-1.2.3.tar.gz"
        checksum = (output / (archive.name + ".sha256")).read_text().split()[0]
        result = self.render(archive, checksum=checksum)
        self.assertEqual(result.returncode, 0, result.stderr)
        for name in ("chimeradb", "chimeradb@10.11"):
            formula = (self.output / (name + ".rb")).read_text()
            self.assertIn('  version "1.2.3"\n', formula)
            self.assertIn('  sha256 "' + checksum + '"\n', formula)
            self.assertNotIn('version "3.0.0"', formula)
        self.assertEqual(self.version.read_text(), "3.0.0\n")

    def test_checksum_must_identify_the_archive_read_for_version(self):
        archive = self.archive([("chimeradb-1.2.3/chimera/VERSION", b"1.2.3\n", tarfile.REGTYPE)])
        self.assert_rejected(self.render(archive, checksum="0" * 64), "does not match --sha256")

    def test_missing_duplicate_and_symlink_version_are_rejected(self):
        version = ("chimeradb-1.2.3/chimera/VERSION", b"1.2.3\n", tarfile.REGTYPE)
        for entries in ([], [version, version], [(version[0], b"", tarfile.SYMTYPE)]):
            with self.subTest(entries=entries):
                self.assert_rejected(self.render(self.archive(entries)), "exactly one regular chimera/VERSION")

    def test_invalid_version_and_mismatched_archive_prefix_are_rejected(self):
        for name, version, message in (
                ("chimeradb-1.2.3/chimera/VERSION", b"1.2.3\n4.5.6", "invalid ChimeraDB version"),
                ("chimeradb-9.9.9/chimera/VERSION", b"1.2.3", "prefix does not match")):
            with self.subTest(name=name, version=version):
                archive = self.archive([(name, version, tarfile.REGTYPE)])
                self.assert_rejected(self.render(archive), message)


if __name__ == "__main__":
    unittest.main(verbosity=2)
