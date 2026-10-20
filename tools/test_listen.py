# SPDX-License-Identifier: Apache-2.0
"""Tests for auracast_listen.py (run: /usr/bin/python3 -m unittest discover -s tools -p 'test_*.py')."""

import unittest

import auracast_listen

PATHS = [
    "/org/bluez/hci0/dev_AA/src0/sid0/bis1/fd0",
    "/org/bluez/hci0/dev_AA/src0/sid0/bis2/fd0",
    "/org/bluez/hci0/dev_AA/src0/sid0/bis3/fd0",
]


class FilterBisTest(unittest.TestCase):
    def test_none_selects_all(self):
        self.assertEqual(auracast_listen.filter_bis(PATHS, None), PATHS)

    def test_subset(self):
        self.assertEqual(auracast_listen.filter_bis(PATHS, "2,3"), PATHS[1:])

    def test_single_with_spaces(self):
        self.assertEqual(auracast_listen.filter_bis(PATHS, " 1 "), PATHS[:1])

    def test_unknown_number_gives_empty(self):
        self.assertEqual(auracast_listen.filter_bis(PATHS, "7"), [])

    def test_bad_spec_raises(self):
        with self.assertRaises(ValueError):
            auracast_listen.filter_bis(PATHS, "a,b")


if __name__ == "__main__":
    unittest.main()
