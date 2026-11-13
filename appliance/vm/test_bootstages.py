"""Tests for bootstages.py: stage recognition on serial text.

The transcripts are assembled from the strings bootstages.py cites, not
recorded from a real boot; C1's first granted run records a real transcript,
which then joins these tests as data.

Run: python3 -I -m unittest discover -s appliance/vm -p 'test_*.py'
"""

import unittest

from bootstages import StageTracker, default_stage_list, strip_escapes

FIRMWARE = b'BdsDxe: loading Boot0001 "UEFI Misc Device" from PciRoot(0x0)\r\n'
LOADER = (b'Consoles: EFI console  \r\n'
          b'FreeBSD/arm64 EFI loader, Revision 3.0\r\n'
          b'   Command line arguments: loader.efi\r\n'
          b'/boot/kernel/kernel text=0x1a0 data=0x1c8 syms=[0x8+0x1]\r\n'
          b'\x1b[1;1H\x1b[2JAutoboot in 10 seconds. [Space] to pause ')
COUNTDOWN_END = b'\rBooting [/boot/kernel/kernel]...               \r\n'
KERNEL = (b'---<<BOOT>>---\r\n'
          b'Copyright (c) 1992-2026 The FreeBSD Project.\r\n'
          b'Trying to mount root from ufs:/dev/gpt/rootfs [rw]...\r\n')
USERLAND = (b'Setting hostuuid: 00000000-0000-0000-0000-000000000000.\r\n'
            b'\r\nFreeBSD/arm64 (freebsd) (ttyu0)\r\n\r\nlogin: ')


def feed_all(tracker: StageTracker, chunks: list) -> None:
    seconds = 0.0
    for chunk in chunks:
        seconds += 1.0
        tracker.feed(chunk, seconds)


class StageRecognition(unittest.TestCase):

    def test_full_boot_records_every_stage_in_order(self) -> None:
        tracker = StageTracker(default_stage_list())
        feed_all(tracker, [FIRMWARE, LOADER, COUNTDOWN_END, KERNEL, USERLAND])
        self.assertEqual(tracker.missing_required(), [])
        self.assertEqual(tracker.seen_seconds['firmware_boot_option'], 1.0)
        self.assertEqual(tracker.seen_seconds['loader_banner'], 2.0)
        self.assertEqual(tracker.seen_seconds['loader_countdown'], 2.0)
        self.assertEqual(tracker.seen_seconds['loader_boot'], 3.0)
        self.assertEqual(tracker.seen_seconds['kernel_first'], 4.0)
        self.assertEqual(tracker.seen_seconds['root_mount'], 4.0)
        self.assertEqual(tracker.seen_seconds['userland_first'], 5.0)
        self.assertEqual(tracker.seen_seconds['login_prompt'], 5.0)
        self.assertEqual(tracker.interval_seconds('loader_banner', 'kernel_first'), 2.0)
        self.assertEqual(tracker.interval_seconds(None, 'login_prompt'), 5.0)

    def test_prompt_without_line_end_is_seen(self) -> None:
        tracker = StageTracker(default_stage_list())
        tracker.feed(b'login: ', 7.5)
        self.assertEqual(tracker.seen_seconds.get('login_prompt'), 7.5)

    def test_line_split_across_chunks_is_timed_at_completion(self) -> None:
        tracker = StageTracker(default_stage_list())
        tracker.feed(b'Trying to mount ro', 1.0)
        self.assertNotIn('root_mount', tracker.seen_seconds)
        tracker.feed(b'ot from ufs:/dev/vtbd0p2 [rw]...\r\n', 2.0)
        self.assertEqual(tracker.seen_seconds['root_mount'], 2.0)

    def test_countdown_lines_do_not_count_as_boot(self) -> None:
        tracker = StageTracker(default_stage_list())
        tracker.feed(b'\rBooting [/boot/kernel/kernel] in 9 seconds... ', 1.0)
        self.assertNotIn('loader_boot', tracker.seen_seconds)

    def test_a_stage_is_recorded_once(self) -> None:
        tracker = StageTracker(default_stage_list())
        tracker.feed(b'---<<BOOT>>---\r\n', 1.0)
        tracker.feed(b'---<<BOOT>>---\r\n', 2.0)
        self.assertEqual(tracker.seen_seconds['kernel_first'], 1.0)

    def test_missing_stages_are_reported_and_intervals_are_absent(self) -> None:
        tracker = StageTracker(default_stage_list())
        feed_all(tracker, [FIRMWARE, LOADER])
        self.assertIn('kernel_first', tracker.missing_required())
        self.assertIsNone(tracker.interval_seconds('loader_banner', 'kernel_first'))

    def test_escape_sequences_are_removed_before_matching(self) -> None:
        self.assertEqual(strip_escapes('\x1b[1;1H\x1b[2JAutoboot in 3 seconds'), 'Autoboot in 3 seconds')

    def test_unterminated_input_is_bounded(self) -> None:
        tracker = StageTracker(default_stage_list())
        tracker.feed(b'x' * 100000, 1.0)
        self.assertLessEqual(len(tracker.unfinished_line), 4096)

    def test_invalid_utf8_does_not_stop_recognition(self) -> None:
        tracker = StageTracker(default_stage_list())
        tracker.feed(b'\xff\xfe garbage\r\n---<<BOOT>>---\r\n', 1.0)
        self.assertEqual(tracker.seen_seconds['kernel_first'], 1.0)


if __name__ == '__main__':
    unittest.main()
