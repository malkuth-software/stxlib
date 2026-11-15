"""Tests for check_surface.py: the FNM_PATHNAME translation is checked
against every pattern of device_rules.h over a fixed name set, and the
listener rule over its cases."""

import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import check_surface  # noqa: E402


class PatternTest(unittest.TestCase):
    def test_star_stops_at_slash(self) -> None:
        regex = check_surface.pattern_regex('pts/*')
        self.assertTrue(regex.match('pts/0'))
        self.assertFalse(regex.match('pts/0/x'))
        self.assertFalse(regex.match('pts'))
        self.assertFalse(check_surface.pattern_regex('bpf*').match('bpf/x'))
        self.assertTrue(check_surface.pattern_regex('bpf*').match('bpf0'))
        self.assertTrue(check_surface.pattern_regex('vtbd?').match('vtbd0'))
        self.assertFalse(check_surface.pattern_regex('vtbd?').match('vtbd10'))
        self.assertTrue(check_surface.pattern_regex('tty[uv]0').match('ttyv0'))
        self.assertFalse(check_surface.pattern_regex('tty[!v]0').match('ttyv0'))

    def test_base_patterns_read(self) -> None:
        patterns = check_surface.base_patterns(check_surface.DEVICE_RULES_H)
        self.assertIn('null', patterns)
        self.assertIn('pts/*', patterns)
        hidden = ['mem', 'kmem', 'io', 'ttyu0', 'input/event0', 'fb0', 'pci', 'klog', 'devctl']
        self.assertEqual(check_surface.check_devices(hidden, patterns), hidden)
        shown = ['null', 'pts/0', 'ufs/STXDATA', 'gpt/stxrootb', 'mmcsd0s2d', 'vtbd0p6', 'fd/0', 'bpf0']
        self.assertEqual(check_surface.check_devices(shown, patterns), [])

    def test_listeners(self) -> None:
        allowed = check_surface.listener_allowed
        self.assertTrue(allowed('sshd', 'tcp4', '*:22', []))
        self.assertFalse(allowed('sshd', 'tcp4', '*:2222', []))
        self.assertTrue(allowed('sh', 'tcp4', '*:8080', ['tcp/8080']))
        self.assertFalse(allowed('sh', 'udp4', '*:8080', ['tcp/8080']))
        self.assertTrue(allowed('dhclient', 'udp4', '*:68', []))
        self.assertFalse(allowed('stxsupervisor', 'tcp4', '*:1', []))

    def test_log(self) -> None:
        with tempfile.NamedTemporaryFile('w', delete=False, suffix='.txt') as stream:
            stream.write('STXSURFACE dev null\r\nSTXSURFACE dev kmem\nSTXSURFACE listen USER COMMAND PID FD PROTO LOCAL ADDRESS FOREIGN ADDRESS\n'
                         'STXSURFACE listen root sshd 700 4 tcp4 *:22 *:*\nSTXSURFACE done\n')
            path = stream.name
        try:
            self.assertEqual(check_surface.main([path]), 1)
            self.assertEqual(check_surface.main([path, '--device', 'kmem']), 0)
        finally:
            os.unlink(path)


if __name__ == '__main__':
    unittest.main()
