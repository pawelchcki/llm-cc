"""Exercise the real controlling-terminal prompt, including libc++ on macOS."""

import subprocess
import sys
import unittest

from terminal_fixture import PROMPT, run_terminal

PROBE = sys.argv.pop(1)
SOURCE = b"scoring input\n"


class TerminalConsentTest(unittest.TestCase):
    def prompt(self, responses, mode="interactive"):
        return run_terminal([PROBE, mode], responses, source=SOURCE)

    def test_one_line_accepts_without_eof_and_preserves_stdin(self):
        for answer in (b"y\n", b"Y\n", b"yes\n", b"YES\n"):
            with self.subTest(answer=answer):
                status, stdout, stderr, _ = self.prompt([answer])
                self.assertEqual(status, 0, stderr)
                self.assertEqual(stdout, b"accepted stdin=" + SOURCE)
                self.assertIn(b"llm-cc: consent accepted", stderr)

    def test_refusal_default_and_terminal_eof(self):
        for answer in (b"n\n", b"\n", b"\x04"):
            with self.subTest(answer=answer):
                status, stdout, stderr, _ = self.prompt([answer])
                self.assertEqual(status, 1)
                self.assertEqual(stdout, b"")
                self.assertIn(b"download declined", stderr)

    def test_model_and_backend_can_prompt_sequentially(self):
        status, stdout, stderr, _ = self.prompt([b"y\n", b"yes\n"], "twice")
        self.assertEqual(status, 0, stderr)
        self.assertEqual(stdout, b"accepted stdin=" + SOURCE)

    def test_without_terminal_requires_yes_and_offline_still_wins(self):
        for mode, status, diagnostic in (
            ("interactive", 1, b"interactive terminal"),
            ("yes", 0, b"consent accepted"),
            ("offline", 1, b"--no-download forbids network access"),
        ):
            with self.subTest(mode=mode):
                result = subprocess.run(
                    [PROBE, mode], input=SOURCE, capture_output=True,
                    start_new_session=True, timeout=5, check=False,
                )
                self.assertEqual(result.returncode, status, result.stderr)
                self.assertIn(diagnostic, result.stderr)
                self.assertNotIn(PROMPT, result.stderr)
                if status == 0:
                    self.assertEqual(result.stdout, b"accepted stdin=" + SOURCE)


if __name__ == "__main__":
    unittest.main()
