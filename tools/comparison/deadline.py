"""Wall-clock deadlines that also interrupt blocking remote-store calls.

On POSIX main threads this uses ITIMER_REAL, so a deadline cannot be swallowed
by ordinary ``except Exception`` retry loops.  Other platforms retain a
cooperative ``check`` fallback; BuildBuddy remote workers are Linux.
"""

from __future__ import annotations

import signal
import threading
import time


class DeadlineExceeded(BaseException):
    """Deliberately derives from BaseException so SDK retries cannot swallow it."""


class Deadline:
    def __init__(self, seconds: float):
        if seconds < 0:
            raise ValueError("deadline must not be negative")
        self.seconds = seconds
        self.started = 0.0
        self.enabled = False
        self.old_handler = None
        self.old_timer = None

    def check(self) -> None:
        if time.monotonic() - self.started >= self.seconds:
            raise DeadlineExceeded("deadline exceeded")

    def __enter__(self):
        self.started = time.monotonic()
        if self.seconds == 0:
            raise DeadlineExceeded("deadline exceeded")
        if (
            hasattr(signal, "setitimer")
            and hasattr(signal, "ITIMER_REAL")
            and threading.current_thread() is threading.main_thread()
        ):
            self.old_handler = signal.getsignal(signal.SIGALRM)
            self.old_timer = signal.getitimer(signal.ITIMER_REAL)
            previous_delay, _ = self.old_timer
            effective = (
                min(self.seconds, previous_delay) if previous_delay else self.seconds
            )
            # Installing the handler first means a failed installation never
            # leaves a changed process-global timer behind.
            signal.signal(signal.SIGALRM, self._expired)
            try:
                signal.setitimer(signal.ITIMER_REAL, effective)
            except BaseException:
                signal.signal(signal.SIGALRM, self.old_handler)
                raise
            self.enabled = True
        return self

    @staticmethod
    def _expired(signum, frame):
        raise DeadlineExceeded("deadline exceeded")

    def __exit__(self, exc_type, exc, traceback):
        if self.enabled:
            elapsed = time.monotonic() - self.started
            previous_delay, previous_interval = self.old_timer
            # Stop the inner timer before restoring its handler: otherwise a
            # SIGALRM can race into an unrelated outer handler.
            signal.setitimer(signal.ITIMER_REAL, 0)
            signal.signal(signal.SIGALRM, self.old_handler)
            remaining = max(0.0, previous_delay - elapsed)
            signal.setitimer(
                signal.ITIMER_REAL,
                remaining,
                previous_interval,
            )
            if previous_delay and remaining == 0 and exc_type is None:
                raise DeadlineExceeded("deadline exceeded")
        return False
