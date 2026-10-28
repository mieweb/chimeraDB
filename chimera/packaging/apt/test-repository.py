#!/usr/bin/env python3
"""Exercise signed APT publication with real disposable GPG keys and .debs.

Run directly on Debian with python3, dpkg-dev, gnupg and apt installed, or pass
--docker on another host. Tests never install packages or create production keys.
"""

import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import pwd
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import unittest


HERE = Path(__file__).resolve().parent
GENERATOR = HERE / "generate-repository.py"
SOURCE_REF = "a" * 40
NEXT_REF = "b" * 40
KEYRING = "chimeradb-archive-keyring.gpg"
SUITES = {"bookworm": ("10.11", "Debian 12 (bookworm)", "1:10.11.14-0+deb12u2"),
          "trixie": ("11.8", "Debian 13 (trixie)", "1:11.8.3-0+deb13u1")}


def run(arguments, **kwargs):
    return subprocess.run([str(arg) for arg in arguments], check=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, **kwargs).stdout


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class RepositoryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.workspace = tempfile.TemporaryDirectory(prefix="chimeradb-apt-test-")
        cls.root = Path(cls.workspace.name)
        cls.root.chmod(0o755)
        cls.gnupg = cls.root / "gnupg"
        cls.gnupg.mkdir(mode=0o700)
        cls.environment = {**os.environ, "GNUPGHOME": str(cls.gnupg), "SOURCE_DATE_EPOCH": "1700000000"}
        for name in ("publisher", "unrelated"):
            run(["gpg", "--batch", "--pinentry-mode", "loopback", "--passphrase", "",
                 "--quick-generate-key", f"Chimera test {name} <{name}@example.invalid>",
                 "rsa2048", "sign", "0"], env=cls.environment)
        keys = run(["gpg", "--batch", "--with-colons", "--list-keys"], env=cls.environment).decode()
        fingerprints = [line.split(":")[9] for line in keys.splitlines() if line.startswith("fpr:")]
        cls.fingerprint, cls.unrelated = fingerprints
        cls.base = {suite: cls.make_set(cls.root / suite, suite) for suite in SUITES}
        cls.repository = cls.root / "initial"
        cls.generate(cls.repository, cls.base)

    @classmethod
    def tearDownClass(cls):
        run(["gpgconf", "--kill", "gpg-agent"], env=cls.environment)
        cls.workspace.cleanup()

    def setUp(self):
        self.case = Path(tempfile.mkdtemp(dir=self.root))

    def tearDown(self):
        shutil.rmtree(self.case)

    @classmethod
    def make_set(cls, directory, suite, version="0.1.0-1", payload="original", wrong_server=False,
                 include_debug=False):
        directory.mkdir(parents=True)
        series, distribution, server_version = SUITES[suite]
        packages = [("chimeradb", "all", f"chimeradb-common (= {version}), chimeradb-plugin (= {version})"),
                    ("chimeradb-common", "amd64", "mariadb-client"),
                    (f"chimeradb-plugin-{series}", "amd64",
                     f"chimeradb-common (= {version}), mariadb-server (= {'1:99.0.0' if wrong_server else server_version})")]
        if include_debug:
            packages.append((f"chimeradb-plugin-{series}-dbgsym", "amd64",
                             f"chimeradb-plugin-{series} (= {version})"))
        for name, architecture, dependencies in packages:
            with tempfile.TemporaryDirectory(dir=cls.root) as temporary:
                tree = Path(temporary)
                (tree / "DEBIAN").mkdir()
                (tree / "DEBIAN/control").write_text(
                    f"Package: {name}\nVersion: {version}\nArchitecture: {architecture}\n"
                    "Maintainer: ChimeraDB tests <test@example.invalid>\n"
                    f"Depends: {dependencies}\nDescription: disposable APT fixture\n")
                content = tree / "usr/share/chimeradb-fixture"
                content.parent.mkdir(parents=True)
                content.write_text(f"{suite} {payload}\n")
                run(["dpkg-deb", "--build", "--root-owner-group", tree,
                     directory / f"{name}_{version}_{architecture}.deb"], env=cls.environment)
        (directory / "build-info.txt").write_text(
            f"ChimeraDB: {version}\nMariaDB package: {server_version}\nMariaDB series: {series}\n"
            f"Architecture: amd64\nDistribution: {distribution}\n")
        cls.manifest(directory)
        return directory

    @staticmethod
    def manifest(directory):
        (directory / "SHA256SUMS").write_text("".join(
            f"{digest(path)}  ./{path.name}\n" for path in sorted(directory.glob("*.deb"))))

    @classmethod
    def command(cls, output, inputs, previous=None, source=SOURCE_REF, key=None):
        command = [sys.executable, GENERATOR, "--source-ref", source,
                   "--signing-key", key or cls.fingerprint, "--output", output,
                   "--timestamp", str(int(time.time()))]
        for suite, directory in inputs.items():
            command += ["--package-set", f"{suite}={directory}"]
        if previous:
            command += ["--previous", previous]
        return command

    @classmethod
    def generate(cls, output, inputs, **kwargs):
        return run(cls.command(output, inputs, **kwargs), env=cls.environment)

    def reject(self, label, inputs=None, **kwargs):
        output = self.case / "rejected-output"
        result = subprocess.run([str(value) for value in self.command(output, inputs or self.base, **kwargs)],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=self.environment)
        self.assertNotEqual(result.returncode, 0, label)
        self.assertFalse(output.exists(), f"{label}: failed publication left an output")
        self.assertFalse(list(self.case.glob(".rejected-output-*")), f"{label}: staging files remain")
        return result.stderr.decode()

    def copy_previous(self):
        previous = self.case / "previous"
        shutil.copytree(self.repository, previous)
        return previous

    def test_both_suites_and_scoped_key_are_signed(self):
        public_keys = run(["gpg", "--batch", "--show-keys", "--with-colons", self.repository / KEYRING],
                          env=self.environment).decode()
        self.assertEqual(public_keys.count("\npub:") + public_keys.startswith("pub:"), 1)
        self.assertIn(self.fingerprint, public_keys)
        self.assertNotIn(self.unrelated, public_keys)
        self.assertEqual(len(list((self.repository / "pool").rglob("*.deb"))), 6)
        for suite in SUITES:
            distribution = self.repository / "dists" / suite
            run(["gpgv", "--keyring", self.repository / KEYRING, distribution / "InRelease"])
            run(["gpgv", "--keyring", self.repository / KEYRING,
                 distribution / "Release.gpg", distribution / "Release"])
            packages = (distribution / "main/binary-amd64/Packages").read_bytes()
            self.assertEqual(packages.count(b"\nFilename: "), 3)
            self.assertIn(f"Filename: pool/{suite}/".encode(), packages)
            other = next(name for name in SUITES if name != suite)
            self.assertNotIn(f"Filename: pool/{other}/".encode(), packages)
            self.assertEqual(gzip.decompress((distribution / "main/binary-amd64/Packages.gz").read_bytes()), packages)
            metadata = json.loads((distribution / "chimeradb.json").read_text())
            self.assertEqual(metadata["package_sets"][0]["source_ref"], SOURCE_REF)
            for path in (distribution / "main/binary-amd64/by-hash/SHA256").iterdir():
                self.assertEqual(path.name, digest(path))

    def test_apt_update_accepts_repository_and_selects_each_suite(self):
        # Exercise actual APT signature/index parsing entirely in disposable
        # state directories. No host sources, package installs or network I/O.
        for suite, (series, _, _) in SUITES.items():
            state = self.case / suite
            (state / "lists/partial").mkdir(parents=True)
            (state / "cache/archives/partial").mkdir(parents=True)
            sources = state / "sources.list"
            sources.write_text(f"deb [signed-by={self.repository / KEYRING}] file:{self.repository} {suite} main\n")
            options = ["-o", f"Dir::Etc::sourcelist={sources}", "-o", "Dir::Etc::sourceparts=-",
                       "-o", "Dir::State::status=/dev/null", "-o", f"Dir::State::lists={state / 'lists'}",
                       "-o", f"Dir::Cache={state / 'cache'}", "-o", "APT::Get::List-Cleanup=0",
                       "-o", f"APT::Sandbox::User={pwd.getpwuid(os.getuid()).pw_name}",
                       "-o", "APT::Architecture=amd64", "-o", "APT::Architectures::=amd64"]
            run(["apt-get", *options, "update"], env=self.environment)
            policy = run(["apt-cache", *options, "policy", "chimeradb", f"chimeradb-plugin-{series}"],
                         env=self.environment).decode()
            self.assertEqual(policy.count("Candidate: 0.1.0-1"), 2)

    def test_upgrade_preserves_packages_versions_other_suite_and_old_by_hash(self):
        new = self.make_set(self.case / "new", "bookworm", version="0.1.0-2")
        output = self.case / "upgraded"
        self.generate(output, {"bookworm": new}, previous=self.repository, source=NEXT_REF)
        for path in (self.repository / "pool").rglob("*.deb"):
            self.assertEqual(path.read_bytes(), (output / path.relative_to(self.repository)).read_bytes())
        for path in (self.repository / "dists/bookworm/main/binary-amd64/by-hash/SHA256").iterdir():
            self.assertEqual(path.read_bytes(), (output / path.relative_to(self.repository)).read_bytes())
        packages = (output / "dists/bookworm/main/binary-amd64/Packages").read_text()
        self.assertEqual(packages.count("\nVersion: 0.1.0-1\n"), 3)
        self.assertEqual(packages.count("\nVersion: 0.1.0-2\n"), 3)
        self.assertEqual(json.loads((output / "dists/trixie/chimeradb.json").read_text()),
                         json.loads((self.repository / "dists/trixie/chimeradb.json").read_text()))

    def test_identical_retry_is_allowed(self):
        output = self.case / "retry"
        self.generate(output, self.base, previous=self.repository)
        self.assertEqual(len(list((output / "pool").rglob("*.deb"))), 6)

    def test_changed_immutable_version_is_rejected(self):
        changed = self.make_set(self.case / "changed", "bookworm", payload="rebuilt")
        message = self.reject("immutable version", {"bookworm": changed}, previous=self.repository)
        self.assertIn("immutable package version changed", message)

    def test_source_commit_rebinding_is_rejected(self):
        message = self.reject("source rebinding", previous=self.repository, source=NEXT_REF)
        self.assertIn("another source commit", message)

    def test_version_rollback_is_rejected(self):
        older = self.make_set(self.case / "older", "bookworm", version="0.0.9-1")
        message = self.reject("rollback", {"bookworm": older}, previous=self.repository)
        self.assertIn("version rollback", message)

    def test_unsigned_previous_repository_is_rejected(self):
        previous = self.copy_previous()
        (previous / "dists/bookworm/InRelease").unlink()
        self.reject("missing signature", previous=previous)

    def test_wrong_signing_key_cannot_retain_previous_repository(self):
        self.reject("wrong signing key", previous=self.repository, key=self.unrelated)

    def test_previous_signed_indices_and_metadata_cannot_be_tampered(self):
        for name in ("Release", "chimeradb.json", "main/binary-amd64/Packages"):
            with self.subTest(name=name):
                previous = self.case / (name.replace("/", "-") + "-previous")
                shutil.copytree(self.repository, previous)
                path = previous / "dists/bookworm" / name
                path.write_bytes(path.read_bytes() + b"tampered\n")
                self.reject(name, previous=previous)

    def test_previous_package_bytes_cannot_be_tampered(self):
        previous = self.copy_previous()
        path = next((previous / "pool/bookworm").rglob("*.deb"))
        path.write_bytes(path.read_bytes() + b"tampered")
        self.reject("prior package bytes", previous=previous)

    def test_previous_symlinks_are_rejected(self):
        previous = self.copy_previous()
        path = previous / "dists/bookworm/Release"
        path.unlink()
        path.symlink_to(self.repository / "dists/bookworm/Release")
        self.reject("prior symlink", previous=previous)

    def test_manifest_extra_files_and_wrong_abi_are_rejected(self):
        directory = self.case / "unlisted"
        shutil.copytree(self.base["bookworm"], directory)
        original = next(directory.glob("*.deb"))
        shutil.copyfile(original, directory / "extra.deb")
        self.reject("unlisted .deb", {"bookworm": directory})
        wrong = self.make_set(self.case / "wrong-server", "bookworm", wrong_server=True)
        self.reject("unpinned MariaDB", {"bookworm": wrong})

    def test_debug_archive_is_verified_and_excluded_from_the_public_repository(self):
        packages = self.make_set(self.case / "debug", "bookworm", include_debug=True)
        output = self.case / "runtime-only"
        self.generate(output, {"bookworm": packages})
        self.assertEqual(len(list((output / "pool").rglob("*.deb"))), 3)
        self.assertNotIn("dbgsym", (output / "dists/bookworm/main/binary-amd64/Packages").read_text())
        debug = next(packages.glob("*dbgsym*.deb"))
        debug.write_bytes(debug.read_bytes() + b"tampered")
        self.reject("debug checksum still required", {"bookworm": packages})

    def test_empty_or_malformed_previous_repository_cannot_reset_history(self):
        empty = self.case / "empty-previous"
        empty.mkdir()
        self.reject("empty previous repository", previous=empty)
        (empty / "dists").mkdir()
        self.reject("no previous distributions", previous=empty)
        (empty / "dists/unsupported").mkdir()
        self.reject("unsupported previous distribution", previous=empty)

    def test_distribution_mismatch_is_rejected(self):
        self.reject("distribution mismatch", {"trixie": self.base["bookworm"]})

    def test_existing_output_is_never_modified(self):
        output = self.case / "existing"
        output.mkdir()
        (output / "sentinel").write_text("preserve")
        result = subprocess.run([str(value) for value in self.command(output, self.base)],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=self.environment)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(list(output.iterdir()), [output / "sentinel"])
        self.assertEqual((output / "sentinel").read_text(), "preserve")

    def test_workflow_passphrase_cache_signs_with_a_protected_key(self):
        home = self.case / "protected-gnupg"
        home.mkdir(mode=0o700)
        environment = {**self.environment, "GNUPGHOME": str(home)}
        passphrase = b"disposable fixture passphrase\n"
        try:
            run(["gpg", "--batch", "--pinentry-mode", "loopback", "--passphrase-fd", "0",
                 "--quick-generate-key", "Protected test <protected@example.invalid>", "rsa2048", "sign", "0"],
                input=passphrase, env=environment)
            listing = run(["gpg", "--batch", "--with-colons", "--list-keys"], env=environment).decode()
            fingerprint = next(line.split(":")[9] for line in listing.splitlines() if line.startswith("fpr:"))
            # Match publish-apt.yml: unlock once through stdin, then the
            # generator uses that isolated agent without receiving a secret.
            probe = self.case / "signing-test"
            probe.write_text("signing test\n")
            run(["gpg", "--batch", "--yes", "--pinentry-mode", "loopback", "--passphrase-fd", "0",
                 "--local-user", fingerprint, "--detach-sign", "--output", probe.with_suffix(".sig"), probe],
                input=passphrase, env=environment)
            output = self.case / "protected-output"
            run(self.command(output, self.base, key=fingerprint), env=environment)
            self.assertTrue((output / "dists/bookworm/InRelease").is_file())
            run(["gpgv", "--keyring", output / KEYRING, output / "dists/bookworm/InRelease"],
                env=environment)
        finally:
            run(["gpgconf", "--kill", "gpg-agent"], env=environment)

    def test_publication_modes_are_readable_with_a_restrictive_umask(self):
        output = self.case / "restrictive-umask"
        run(self.command(output, self.base), env=self.environment, umask=0o077)
        for path in [output, *output.rglob("*")]:
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o755 if path.is_dir() else 0o644,
                             str(path.relative_to(output)))

    def test_caller_gpg_preferences_cannot_change_the_repository_keyring(self):
        configuration = self.gnupg / "gpg.conf"
        self.assertFalse(configuration.exists())
        try:
            configuration.write_text(f"armor\ndefault-key {self.unrelated}\ndigest-algo SHA1\n")
            output = self.case / "gpg-preferences"
            self.generate(output, self.base)
            self.assertNotIn(b"BEGIN PGP PUBLIC KEY", (output / KEYRING).read_bytes())
            run(["gpgv", "--keyring", output / KEYRING, output / "dists/bookworm/InRelease"])
        finally:
            configuration.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--docker", action="store_true", help="run in a disposable Debian bookworm container")
    args, unittest_args = parser.parse_known_args()
    if args.docker:
        repository = HERE.parents[2]
        command = ["docker", "run", "--rm", "--volume", f"{repository}:/work:ro", "--workdir", "/work",
                   "debian:bookworm", "bash", "-ec",
                   "apt-get update -qq && apt-get install -y -qq --no-install-recommends "
                   "python3 dpkg-dev gnupg ca-certificates >/dev/null && "
                   "python3 chimera/packaging/apt/test-repository.py -v"]
        return subprocess.call(command)
    missing = [name for name in ("dpkg-deb", "dpkg-scanpackages", "gpg", "gpgv", "gpgconf", "apt-get", "apt-cache")
               if not shutil.which(name)]
    if missing:
        parser.error(f"missing {', '.join(missing)}; use --docker to run on a non-Debian host")
    unittest.main(argv=[sys.argv[0], *unittest_args])
    return 0


if __name__ == "__main__":
    sys.exit(main())
