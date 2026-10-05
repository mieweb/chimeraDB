#!/usr/bin/env python3
"""Exercise service initialization recovery with fake MariaDB commands.

No listeners, Homebrew installation, or existing data directories are used.
Run with: python3 chimera/packaging/homebrew/test-service.py
"""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest


SERVICE = Path(__file__).with_name("service.sh")
MOCK = r'''
import json
import os
from pathlib import Path
import signal
import sys
import time

name = Path(sys.argv[0]).name
data = Path(os.environ["CHIMERA_DATA_DIR"])
marker = data / ".chimera-initializing"

def event(action):
    with open(os.environ["MOCK_EVENTS"], "a") as output:
        output.write(json.dumps({"action": action, "marker": marker.exists()}) + "\n")

def terminate(signum, frame):
    event(name + "-terminated")
    sys.exit(0)

signal.signal(signal.SIGTERM, terminate)
if name == "mariadbd":
    if "--version" in sys.argv:
        print("mariadbd Ver 11.8.9-MariaDB")
    else:
        event("server-start")
        while True:
            time.sleep(0.02)
elif name == "mariadb-install-db":
    event("install-invoked")
    (data / "mysql").mkdir()
    (data / "mysql/partial-data").write_text("preserve me")
    event("install-start")
    mode = os.environ.get("MOCK_INSTALL_MODE", "success")
    if mode == "fail":
        sys.exit(17)
    while mode == "block":
        time.sleep(0.02)
    event("install-complete")
elif name == "chimeradb":
    event("setup-start")
    mode = os.environ.get("MOCK_SETUP_MODE", "success")
    if mode == "fail":
        sys.exit(27)
    while mode == "block" and not Path(os.environ["MOCK_RELEASE_SETUP"]).exists():
        time.sleep(0.02)
    event("setup-complete")
elif name != "mariadb":
    sys.exit("unexpected mock command " + name)
'''

# Pause at a specific top-level shell command without production test hooks.
# The process has already evaluated earlier commands when the barrier appears.
PAUSE_HOOK = r'''
pause_at_command() {
    if [[ $1 == "$MOCK_PAUSE_COMMAND" && ! -e $MOCK_PAUSED ]]; then
        : > "$MOCK_PAUSED"
        until [[ -e $MOCK_CONTINUE ]]; do sleep 0.02; done
    fi
}
trap 'pause_at_command "$BASH_COMMAND"' DEBUG
'''


class InitializationTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="chimeradb-service-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.data = self.root / "data"
        self.marker = self.data / ".chimera-initializing"
        self.events_file = self.root / "events.jsonl"
        self.pause_hook = self.root / "pause.bash"
        self.pause_hook.write_text(PAUSE_HOOK)
        self.paused = self.root / "paused"
        self.resume = self.root / "resume"
        maria = self.root / "mariadb"
        prefix = self.root / "chimeradb"
        (maria / "bin").mkdir(parents=True)
        (prefix / "libexec").mkdir(parents=True)
        metadata = prefix / "share/chimeradb"
        metadata.mkdir(parents=True)
        (metadata / "mariadb-version").write_text("11.8.9\n")
        (metadata / "mariadb-prefix").write_text(str(maria) + "\n")
        for command in (maria / "bin/mariadb-install-db", maria / "bin/mariadbd",
                        maria / "bin/mariadb", prefix / "libexec/chimeradb"):
            command.write_text("#!" + sys.executable + "\n" + MOCK)
            command.chmod(0o755)
        self.env = dict(os.environ, CHIMERA_MARIADB_PREFIX=str(maria),
                        CHIMERA_PREFIX=str(prefix), CHIMERA_DATA_DIR=str(self.data),
                        CHIMERA_DEFAULTS_FILE=str(self.root / "config.cnf"),
                        MOCK_EVENTS=str(self.events_file),
                        MOCK_RELEASE_SETUP=str(self.root / "release-setup"))

    def events(self):
        if not self.events_file.exists():
            return []
        return [json.loads(line) for line in self.events_file.read_text().splitlines()]

    def count(self, action):
        return sum(event["action"] == action for event in self.events())

    def start(self, **settings):
        process = subprocess.Popen(["bash", str(SERVICE)],
                                   env=dict(self.env, **settings),
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   text=True, start_new_session=True)
        self.addCleanup(self.stop, process)
        return process

    def stop(self, process):
        if process.poll() is None:
            process.terminate()
        try:
            output, _ = process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            output, _ = process.communicate(timeout=5)
        return output

    def start_paused_before(self, command):
        process = self.start(BASH_ENV=str(self.pause_hook),
                             MOCK_PAUSE_COMMAND=command,
                             MOCK_PAUSED=str(self.paused),
                             MOCK_CONTINUE=str(self.resume))
        self.wait_for(self.paused.exists)
        self.assertIsNone(process.poll())
        return process

    def wait_for(self, predicate):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if predicate():
                return
            time.sleep(0.02)
        self.fail("timed out waiting for mock service state")

    def assert_retry_refused(self):
        before = self.events()
        process = self.start()
        output, _ = process.communicate(timeout=5)
        self.assertNotEqual(process.returncode, 0, output)
        self.assertIn("previous initialization did not finish", output)
        self.assertEqual(self.events(), before)
        self.assertTrue(self.marker.exists())
        self.assertEqual((self.data / "mysql/partial-data").read_text(), "preserve me")

    def test_failed_install_leaves_partial_mysql_and_refuses_retry(self):
        process = self.start(MOCK_INSTALL_MODE="fail")
        output, _ = process.communicate(timeout=5)
        self.assertEqual(process.returncode, 17, output)
        self.assertEqual(self.count("server-start"), 0)
        self.assert_retry_refused()

    def test_terminated_install_is_stopped_and_refuses_retry(self):
        process = self.start(MOCK_INSTALL_MODE="block")
        self.wait_for(lambda: self.count("install-start") == 1)
        output = self.stop(process)
        self.assertEqual(process.returncode, 143, output)
        self.assertEqual(self.count("mariadb-install-db-terminated"), 1)
        self.assertEqual(self.count("server-start"), 0)
        self.assert_retry_refused()

    def test_killed_install_leaves_marker_and_refuses_retry(self):
        process = self.start(MOCK_INSTALL_MODE="block")
        self.wait_for(lambda: self.count("install-start") == 1)
        os.killpg(process.pid, signal.SIGKILL)
        process.communicate(timeout=5)
        self.assert_retry_refused()

    def test_failed_catalog_setup_keeps_marker_and_stops_server(self):
        process = self.start(MOCK_SETUP_MODE="fail")
        output, _ = process.communicate(timeout=5)
        self.assertEqual(process.returncode, 27, output)
        self.assertEqual(self.count("mariadbd-terminated"), 1)
        self.assert_retry_refused()

    def test_nonempty_uninitialized_directory_is_preserved(self):
        self.data.mkdir()
        retained = self.data / ".existing-data"
        retained.write_bytes(b"do not overwrite")
        process = self.start()
        output, _ = process.communicate(timeout=5)
        self.assertNotEqual(process.returncode, 0, output)
        self.assertIn("data directory is nonempty", output)
        self.assertEqual(retained.read_bytes(), b"do not overwrite")
        self.assertFalse(self.marker.exists())
        self.assertEqual(self.events(), [])

    def test_marker_clears_only_after_setup_and_restart_skips_install(self):
        process = self.start(MOCK_SETUP_MODE="block")
        self.wait_for(lambda: self.count("setup-start") == 1)
        self.assertTrue(self.marker.exists())
        self.assertTrue(all(event["marker"] for event in self.events()))
        Path(self.env["MOCK_RELEASE_SETUP"]).touch()
        self.wait_for(lambda: not self.marker.exists())
        self.assertIsNone(process.poll())
        self.stop(process)
        restarted = self.start()
        self.wait_for(lambda: self.count("setup-complete") == 2)
        self.assertEqual(self.count("install-start"), 1)
        self.assertFalse(self.marker.exists())
        self.stop(restarted)

    def test_existing_unmarked_mysql_directory_is_compatible(self):
        (self.data / "mysql").mkdir(parents=True)
        process = self.start()
        self.wait_for(lambda: self.count("setup-complete") == 1)
        self.assertEqual(self.count("install-start"), 0)
        self.assertFalse(self.marker.exists())
        self.stop(process)

    def test_delayed_contender_refuses_data_created_after_empty_snapshot(self):
        contender = self.start_paused_before('[[ -z $existing_entry ]]')
        winner = self.start()
        self.wait_for(lambda: self.count("setup-complete") == 1
                      and not self.marker.exists())
        self.stop(winner)
        before = self.events()

        self.resume.touch()
        output, _ = contender.communicate(timeout=5)
        self.assertNotEqual(contender.returncode, 0, output)
        self.assertIn("data directory changed before initialization was claimed", output)
        self.assertEqual(self.events(), before)
        self.assertEqual(self.count("install-invoked"), 1)
        self.assertFalse(self.marker.exists())
        self.assertEqual((self.data / "mysql/partial-data").read_text(), "preserve me")

    def test_existing_mysql_branch_rechecks_concurrent_initialization_marker(self):
        contender = self.start_paused_before('[[ ! -d $CHIMERA_DATA_DIR/mysql ]]')
        winner = self.start(MOCK_INSTALL_MODE="block")
        self.wait_for(lambda: self.count("install-start") == 1)
        before = self.events()

        self.resume.touch()
        self.wait_for(lambda: contender.poll() is not None
                      or self.count("server-start") > 0)
        self.assertEqual(self.count("server-start"), 0)
        output, _ = contender.communicate(timeout=5)
        self.assertNotEqual(contender.returncode, 0, output)
        self.assertIn("previous initialization did not finish", output)
        self.assertEqual(self.events(), before)
        self.assertEqual(self.count("install-invoked"), 1)
        self.assertEqual(self.count("server-start"), 0)
        self.assertTrue(self.marker.exists())
        self.assertEqual((self.data / "mysql/partial-data").read_text(), "preserve me")
        self.stop(winner)


if __name__ == "__main__":
    unittest.main(verbosity=2)
