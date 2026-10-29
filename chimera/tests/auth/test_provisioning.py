#!/usr/bin/env python3
"""Password preparation and server-free provisioning client regressions."""
import sys
sys.dont_write_bytecode = True
import importlib.util
import hashlib
import hmac
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('provision', Path(__file__).resolve().parents[2] / 'scripts/mongo-auth.py')
provision = importlib.util.module_from_spec(spec)
spec.loader.exec_module(provision)


class PasswordPreparation(unittest.TestCase):
    def test_rfc4013_examples(self):
        self.assertEqual(provision.saslprep('I\u00ADX'), b'IX')
        self.assertEqual(provision.saslprep('\u00AA'), b'a')
        self.assertEqual(provision.saslprep('\u2168'), b'IX')
        self.assertEqual(provision.saslprep('a\u00A0b'), b'a b')
        self.assertEqual(provision.saslprep('USER'), b'USER')
        for bad in ('', '\u00AD', '\x07', 'a\nb', '\u0221', '\u0627\u0031'):
            with self.subTest(password=repr(bad)), self.assertRaises(ValueError):
                provision.saslprep(bad)

    def test_sql_literals_cannot_inject_or_lose_bytes(self):
        self.assertEqual(provision.literal("'\\\0"), "X'275c00'")
        self.assertEqual(provision.literal(b'\xff'), "X'ff'")


class ClientForwarding(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='chimera provisioning ')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.calls = self.root / 'client calls.jsonl'
        self.client = self.root / 'fake mariadb'
        self.client.write_text('#!' + sys.executable + '\n' + '''
import json
import os
import sys
statement = sys.stdin.read()
with open(os.environ['FAKE_SQL_CALLS'], 'a') as output:
    output.write(json.dumps({'argv': sys.argv[1:], 'sql': statement}) + '\\n')
if statement.startswith('SELECT COUNT(*) FROM mysql.global_priv'):
    print('1')
''')
        self.client.chmod(0o755)
        self.env = dict(os.environ, FAKE_SQL_CALLS=str(self.calls))
        self.env.pop('CHIMERA_DEFAULTS_FILE', None)
        self.connection = ['--socket=' + str(self.root / 'Chimera data/mysql.sock'),
                           '--user=SQL administrator', '--protocol=socket']

    def invoke(self, options=(), *, defaults=None, remove=False):
        env = dict(self.env)
        if defaults is not None:
            env['CHIMERA_DEFAULTS_FILE'] = defaults
        command = [sys.executable, provision.__file__, '--client', str(self.client),
                   '--user', 'alice', '--password-stdin']
        if remove:
            command.append('--remove')
        result = subprocess.run([*command, '--', *options], env=env, input='Mongo secret\n',
                                capture_output=True, text=True, timeout=10)
        return result

    def assert_calls(self, expected, *, remove=False):
        calls = list(map(json.loads, self.calls.read_text().splitlines()))
        self.assertEqual(len(calls), 1 if remove else 2)
        for call in calls:
            self.assertEqual(call['argv'], [*expected, '--batch', '--skip-column-names'])
            self.assertNotIn('Mongo secret', json.dumps(call))
        if remove:
            self.assertIn('DELETE FROM chimera_auth.credentials', calls[0]['sql'])
        else:
            self.assertIn('SELECT COUNT(*) FROM mysql.global_priv', calls[0]['sql'])
            self.assertIn('REPLACE INTO chimera_auth.credentials', calls[1]['sql'])

    def test_formula_defaults_are_first_and_preserve_explicit_socket_and_user(self):
        defaults = str(self.root / 'Homebrew config/chimeradb.cnf')
        result = self.invoke(self.connection, defaults=defaults)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_calls(['--defaults-file=' + defaults, *self.connection])

    def test_no_environment_default_preserves_client_options(self):
        result = self.invoke(self.connection)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_calls(self.connection)

    def test_provisioned_verifier_uses_mongo_c_driver_salt_format(self):
        result = self.invoke(self.connection)
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = list(map(json.loads, self.calls.read_text().splitlines()))
        values = calls[-1]['sql'].rsplit(' VALUES (', 1)[1].removesuffix(');').split(',')
        salt = bytes.fromhex(values[3][2:-1])
        self.assertEqual(len(salt), 28, 'libmongoc requires a 28-byte SCRAM-SHA-256 salt')
        salted = hashlib.pbkdf2_hmac('sha256', b'Mongo secret', salt, int(values[4]))
        self.assertEqual(bytes.fromhex(values[5][2:-1]),
                         hashlib.sha256(hmac.digest(salted, b'Client Key', 'sha256')).digest())
        self.assertEqual(bytes.fromhex(values[6][2:-1]),
                         hmac.digest(salted, b'Server Key', 'sha256'))

    def test_explicit_defaults_replace_formula_default_in_equals_and_split_forms(self):
        for option in ('--defaults-file', '--defaults-extra-file'):
            for split in (False, True):
                with self.subTest(option=option, split=split):
                    self.calls.unlink(missing_ok=True)
                    explicit = str(self.root / 'Other instance/config.cnf')
                    selection = [option, explicit] if split else [option + '=' + explicit]
                    result = self.invoke([*self.connection, *selection], defaults='/formula/default.cnf')
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assert_calls([option + '=' + explicit, *self.connection])

    def test_explicit_file_and_extra_file_precede_connection_options(self):
        options = [*self.connection, '--defaults-extra-file=/extra config.cnf',
                   '--defaults-file=/primary config.cnf']
        result = self.invoke(options, defaults='/formula/default.cnf')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_calls(['--defaults-file=/primary config.cnf',
                           '--defaults-extra-file=/extra config.cnf', *self.connection])

    def test_no_defaults_disables_formula_config_and_is_first(self):
        result = self.invoke([*self.connection, '--no-defaults'], defaults='/formula/default.cnf')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_calls(['--no-defaults', *self.connection])

    def test_revoke_uses_formula_config(self):
        result = self.invoke(self.connection, defaults='/formula config.cnf', remove=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_calls(['--defaults-file=/formula config.cnf', *self.connection], remove=True)

    def test_invalid_or_conflicting_defaults_do_not_connect(self):
        for options in (['--no-defaults', '--defaults-file=/other.cnf'],
                        ['--defaults-extra-file=/other.cnf', '--no-defaults'],
                        ['--defaults-file'], ['--defaults-extra-file='],
                        ['--defaults-file', '--socket=/other.sock'],
                        ['--defaults-file=/first.cnf', '--defaults-file=/second.cnf']):
            with self.subTest(options=options):
                result = self.invoke(options, defaults='/formula/default.cnf')
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(self.calls.exists())


if __name__ == '__main__':
    unittest.main()
