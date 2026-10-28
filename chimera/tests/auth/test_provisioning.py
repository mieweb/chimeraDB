#!/usr/bin/env python3
"""RFC 4013 normalization and verifier provisioning input regression cases."""
import sys
sys.dont_write_bytecode = True
import importlib.util
from pathlib import Path
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


if __name__ == '__main__':
    unittest.main()
