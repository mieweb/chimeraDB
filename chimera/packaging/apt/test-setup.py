#!/usr/bin/env python3
"""Exercise bootstrap trust and host writes without modifying the test host.

The helper is sourced, then its host reader and file destinations are replaced
in the test shell. Production has no environment-variable overrides. GPG trust
checks use real disposable certificates when gpg is available.
"""
import os
from pathlib import Path
import shutil
import stat
import subprocess
import tempfile
import unittest


SCRIPT = Path(__file__).with_name("setup-repository.sh").resolve()
WRAPPER = r'''
source "$SETUP_SCRIPT"
read_host() {
  host_id=$TEST_ID host_version=$TEST_VERSION host_suite=$TEST_SUITE
  host_arch=$TEST_ARCH host_uid=$TEST_UID
}
eval "$(declare -f write_repository_files | sed '1s/write_repository_files/real_write_repository_files/')"
write_repository_files() {
  real_write_repository_files "$1" "$2" "$TEST_ETC/keyrings/chimeradb.gpg" "$TEST_ETC/sources.list.d/chimeradb.sources"
}
curl() {
  printf 'curl %s\n' "$*" >> "$TEST_EVENTS"
  [[ ${TEST_CURL_FAIL:-0} == 0 ]] || return 22
  local output=
  while (($#)); do
    if [[ $1 == --output ]]; then output=$2; shift 2; else shift; fi
  done
  cp "$TEST_KEY" "$output"
}
apt-get() {
  printf 'apt-get %s\n' "$*" >> "$TEST_EVENTS"
  [[ $* == 'update --error-on=any' && ${TEST_APT_FAIL:-0} == 0 ]]
}
main "$@"
'''


class SetupCase(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="chimera-apt-setup-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.etc = self.root / "etc"
        self.etc.mkdir()
        self.events = self.root / "events"
        self.environment = {
            **os.environ,
            "SETUP_SCRIPT": str(SCRIPT), "TEST_ETC": str(self.etc),
            "TEST_EVENTS": str(self.events), "TEST_ID": "debian",
            "TEST_VERSION": "12", "TEST_SUITE": "bookworm",
            "TEST_ARCH": "amd64", "TEST_UID": "0",
        }

    def run_setup(self, *arguments, **environment):
        return subprocess.run(
            ["bash", "-c", WRAPPER, "test-setup", *arguments],
            env={**self.environment, **environment}, text=True, capture_output=True,
        )

    def assert_no_host_writes(self):
        self.assertEqual(list(self.etc.iterdir()), [])
        self.assertFalse(self.events.exists())


class InputValidation(SetupCase):
    def test_missing_short_and_invalid_fingerprint(self):
        for fingerprint in (None, "12345678", "G" * 40):
            with self.subTest(fingerprint=fingerprint):
                arguments = [] if fingerprint is None else ["--key-fingerprint", fingerprint]
                result = self.run_setup(*arguments)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("40-digit", result.stderr)
                self.assert_no_host_writes()

    def test_reject_unsupported_hosts_before_network_or_writes(self):
        for fixture in (
            {"TEST_ID": "ubuntu"}, {"TEST_ARCH": "arm64"},
            {"TEST_SUITE": "trixie"}, {"TEST_VERSION": "11"},
        ):
            with self.subTest(fixture=fixture):
                result = self.run_setup("--key-fingerprint", "A" * 40, **fixture)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("supported hosts", result.stderr)
                self.assert_no_host_writes()

    def test_require_root_before_network_or_writes(self):
        result = self.run_setup("--key-fingerprint", "A" * 40, TEST_UID="1000")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("run this helper as root", result.stderr)
        self.assert_no_host_writes()

    def test_reject_unsafe_repository_urls(self):
        for url in (
            "http://example.com/apt", "file:///etc/apt", "https://x/apt\nTypes: deb-src",
            "https://user:password@example.com/apt", "https://example.com/apt?x=y",
            "https://example.com/apt#fragment",
        ):
            with self.subTest(url=url):
                result = self.run_setup("--key-fingerprint", "A" * 40, "--repository-url", url)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("HTTPS base URL", result.stderr)
                self.assert_no_host_writes()

    def test_help_requires_neither_root_nor_fingerprint(self):
        result = self.run_setup("--help", TEST_UID="1000")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--key-fingerprint", result.stdout)
        self.assert_no_host_writes()


@unittest.skipUnless(shutil.which("gpg"), "real GPG is required for certificate checks")
class KeyAndConfiguration(SetupCase):
    @classmethod
    def setUpClass(cls):
        cls.fixtures = tempfile.TemporaryDirectory(prefix="chimera-apt-certificates-")
        cls.addClassCleanup(cls.fixtures.cleanup)
        cls.fixture_root = Path(cls.fixtures.name)
        cls.key_home = cls.fixture_root / "gnupg"
        cls.key_home.mkdir(mode=0o700)
        if shutil.which("gpgconf"):
            cls.addClassCleanup(
                subprocess.run, ["gpgconf", "--homedir", str(cls.key_home), "--kill", "gpg-agent"],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
            )
        cls.gpg("--pinentry-mode", "loopback", "--passphrase", "", "--quick-generate-key",
                "Chimera setup fixture <fixture@example.invalid>", "rsa2048", "sign", "0")
        listing = cls.gpg("--with-colons", "--list-keys").stdout.decode()
        cls.fingerprint = next(line.split(":")[9] for line in listing.splitlines() if line.startswith("fpr:"))
        cls.gpg("--pinentry-mode", "loopback", "--passphrase", "", "--quick-add-key",
                cls.fingerprint, "rsa2048", "sign", "0")
        listing = cls.gpg("--with-colons", "--with-subkey-fingerprint", "--list-keys").stdout.decode()
        cls.subkey_fingerprint = [line.split(":")[9] for line in listing.splitlines() if line.startswith("fpr:")][1]
        cls.good_key = cls.fixture_root / "good.gpg"
        cls.good_key.write_bytes(cls.gpg("--export", cls.fingerprint).stdout)
        cls.gpg("--pinentry-mode", "loopback", "--passphrase", "", "--quick-generate-key",
                "Unrelated fixture <other@example.invalid>", "rsa2048", "sign", "0")
        cls.bundle = cls.fixture_root / "bundle.gpg"
        cls.bundle.write_bytes(cls.gpg("--export").stdout)
        cls.corrupt = cls.fixture_root / "corrupt.gpg"
        cls.corrupt.write_text("not an OpenPGP certificate\n")

    @classmethod
    def gpg(cls, *arguments):
        return subprocess.run(
            ["gpg", "--batch", "--no-options", "--homedir", str(cls.key_home), *arguments],
            capture_output=True, check=True,
        )

    def setUp(self):
        super().setUp()
        self.environment["TEST_KEY"] = str(self.good_key)

    def setup_repository(self, **environment):
        return self.run_setup("--key-fingerprint", self.fingerprint, **environment)

    def test_each_supported_suite_uses_scoped_key_and_native_series(self):
        for suite, version, series in (("bookworm", "12", "10.11"), ("trixie", "13", "11.8")):
            with self.subTest(suite=suite):
                suite_etc = self.root / suite
                suite_etc.mkdir()
                result = self.setup_repository(TEST_SUITE=suite, TEST_VERSION=version, TEST_ETC=str(suite_etc))
                self.assertEqual(result.returncode, 0, result.stderr)
                key = suite_etc / "keyrings/chimeradb.gpg"
                source = suite_etc / "sources.list.d/chimeradb.sources"
                text = source.read_text()
                self.assertIn(f"Suites: {suite}\n", text)
                self.assertIn("Architectures: amd64\n", text)
                self.assertIn("Signed-By: /etc/apt/keyrings/chimeradb.gpg\n", text)
                self.assertEqual(stat.S_IMODE(key.stat().st_mode), 0o644)
                self.assertEqual(stat.S_IMODE(source.stat().st_mode), 0o644)
                self.assertIn(f"MariaDB {series}", result.stdout)
                self.assertIn("apt install chimeradb", result.stdout)
                self.assertIn("systemctl restart mariadb", result.stdout)
        events = self.events.read_text()
        self.assertEqual(events.count("apt-get update --error-on=any\n"), 2)
        self.assertIn("--proto =https --proto-redir =https", events)
        self.assertIn("/apt/chimeradb-archive-keyring.gpg", events)
        self.assertNotIn("apt-get install", events)

    def test_idempotent_setup_keeps_managed_files_unchanged(self):
        first = self.setup_repository()
        self.assertEqual(first.returncode, 0, first.stderr)
        files = list(self.etc.rglob("*.gpg")) + list(self.etc.rglob("*.sources"))
        before = [(path.read_bytes(), path.stat().st_mtime_ns) for path in files]
        for path in files:
            path.chmod(0o600)
        second = self.setup_repository()
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertEqual(before, [(path.read_bytes(), path.stat().st_mtime_ns) for path in files])
        self.assertTrue(all(stat.S_IMODE(path.stat().st_mode) == 0o644 for path in files))

    def test_mismatched_subkey_multiple_and_corrupt_keys_fail_before_host_writes(self):
        for fingerprint, key in (
            ("A" * 40, self.good_key), (self.subkey_fingerprint, self.good_key),
            (self.fingerprint, self.bundle), (self.fingerprint, self.corrupt),
        ):
            with self.subTest(fingerprint=fingerprint, key=key.name):
                result = self.run_setup("--key-fingerprint", fingerprint, TEST_KEY=str(key))
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(list(self.etc.iterdir()), [])
                self.assertNotIn("apt-get", self.events.read_text())

    def test_download_failure_leaves_host_untouched(self):
        result = self.setup_repository(TEST_CURL_FAIL="1")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(list(self.etc.iterdir()), [])
        self.assertNotIn("apt-get", self.events.read_text())

    def test_divergent_source_is_preserved_without_installing_key(self):
        source = self.etc / "sources.list.d/chimeradb.sources"
        source.parent.mkdir()
        source.write_text("existing source\n")
        result = self.setup_repository()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("differs", result.stderr)
        self.assertEqual(source.read_text(), "existing source\n")
        self.assertFalse((self.etc / "keyrings").exists())
        self.assertNotIn("apt-get", self.events.read_text())

    def test_divergent_key_is_preserved_without_installing_source(self):
        key = self.etc / "keyrings/chimeradb.gpg"
        key.parent.mkdir()
        key.write_bytes(b"previous key")
        result = self.setup_repository()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("differs", result.stderr)
        self.assertEqual(key.read_bytes(), b"previous key")
        self.assertFalse((self.etc / "sources.list.d").exists())

    def test_symlink_is_rejected_without_changing_target(self):
        target = self.root / "target"
        target.write_text("do not change")
        key = self.etc / "keyrings/chimeradb.gpg"
        key.parent.mkdir()
        key.symlink_to(target)
        result = self.setup_repository()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("symlink", result.stderr)
        self.assertEqual(target.read_text(), "do not change")
        self.assertFalse((self.etc / "sources.list.d").exists())

    def test_custom_https_url_and_lowercase_fingerprint(self):
        result = self.run_setup("--key-fingerprint", self.fingerprint.lower(),
                                "--repository-url", "https://packages.example.invalid/chimera/")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("URIs: https://packages.example.invalid/chimera\n",
                      (self.etc / "sources.list.d/chimeradb.sources").read_text())

    def test_apt_failure_does_not_print_install_instructions(self):
        result = self.setup_repository(TEST_APT_FAIL="1")
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("apt install chimeradb", result.stdout)
        self.assertTrue((self.etc / "sources.list.d/chimeradb.sources").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
