"""Tests for guest_session.py's command-file handling (no VM, no QEMU)."""

import os
import tempfile
import unittest

from guest_session import SessionError, parse_defines, read_commands


class CommandFiles(unittest.TestCase):

    def write(self, text: str) -> str:
        handle, path = tempfile.mkstemp(suffix='.txt')
        with os.fdopen(handle, 'w') as stream:
            stream.write(text)
        self.addCleanup(os.remove, path)
        return path

    def test_comments_and_blank_lines_are_skipped_and_placeholders_replaced(self) -> None:
        path = self.write('# comment\n\nfetch -o /k {http}/kernel\ntest x = "{sha}"\n')
        commands = read_commands(path, {'http': 'http://10.0.2.2:9', 'sha': 'ab'})
        self.assertEqual(commands, ['fetch -o /k http://10.0.2.2:9/kernel', 'test x = "ab"'])

    def test_unknown_braces_are_left_alone(self) -> None:
        path = self.write('echo ${HOME}\n')
        self.assertEqual(read_commands(path, {}), ['echo ${HOME}'])

    def test_a_command_holding_the_marker_is_refused(self) -> None:
        path = self.write('echo @@DONE 0@@\n')
        with self.assertRaises(SessionError):
            read_commands(path, {})

    def test_defines_take_plain_names_only(self) -> None:
        self.assertEqual(parse_defines(['kernel_sha256=00ff']), {'kernel_sha256': '00ff'})
        with self.assertRaises(SessionError):
            parse_defines(['http=x'])
        with self.assertRaises(SessionError):
            parse_defines(['no-equals'])
        with self.assertRaises(SessionError):
            parse_defines(['bad name=1'])


if __name__ == '__main__':
    unittest.main()
