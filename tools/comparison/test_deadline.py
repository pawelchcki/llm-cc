import signal
import threading
import time
import unittest

from .deadline import Deadline, DeadlineExceeded


class DeadlineTest(unittest.TestCase):
    def test_zero_deadline_is_immediate(self):
        with self.assertRaises(DeadlineExceeded):
            with Deadline(0):
                pass

    def test_inner_deadline_cannot_extend_outer_timer(self):
        if not hasattr(signal, "setitimer"):
            self.skipTest("no POSIX interval timer")
        started = time.monotonic()
        with self.assertRaises(DeadlineExceeded):
            with Deadline(0.05):
                with Deadline(1):
                    time.sleep(0.2)
        self.assertLess(time.monotonic() - started, 0.15)

    def test_non_main_thread_does_not_change_process_timer(self):
        if not hasattr(signal, "getitimer"):
            self.skipTest("no POSIX interval timer")
        before = signal.getitimer(signal.ITIMER_REAL)
        errors = []

        def invoke():
            try:
                with Deadline(0.01):
                    pass
            except BaseException as error:
                errors.append(error)

        thread = threading.Thread(target=invoke)
        thread.start()
        thread.join()
        self.assertEqual(errors, [])
        self.assertEqual(signal.getitimer(signal.ITIMER_REAL), before)
