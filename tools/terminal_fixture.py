"""Run a command with redirected stdin and a separate controlling terminal."""

import errno
import fcntl
import os
import pty
import select
import subprocess
import termios
import time


PROMPT = b"Download? [y/N] "


def run_terminal(command, responses, *, source=b"", env=None, timeout=5):
    master, slave = pty.openpty()

    def controlling_terminal():
        os.setsid()
        fcntl.ioctl(slave, termios.TIOCSCTTY, 0)

    try:
        with subprocess.Popen(
            command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, env=env, pass_fds=(slave,),
            preexec_fn=controlling_terminal,
        ) as process:
            os.close(slave)
            slave = None
            process.stdin.write(source)
            process.stdin.close()
            process.stdin = None
            transcript = b""
            sent = 0
            deadline = time.monotonic() + timeout
            try:
                while process.poll() is None:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise TimeoutError(
                            f"prompt hung after {sent} answers: {transcript!r}"
                        )
                    if not select.select([master], [], [], min(remaining, 0.1))[0]:
                        continue
                    try:
                        chunk = os.read(master, 4096)
                    except OSError as error:
                        if error.errno != errno.EIO:
                            raise
                        process.wait(timeout=remaining)
                        break
                    if not chunk:
                        process.wait(timeout=remaining)
                        break
                    transcript += chunk
                    if transcript.count(PROMPT) > sent and sent < len(responses):
                        os.write(master, responses[sent])
                        sent += 1
                stdout, stderr = process.communicate(timeout=1)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.communicate()
            if sent != len(responses):
                raise AssertionError(f"sent {sent}/{len(responses)} answers: {transcript!r}")
            return process.returncode, stdout, stderr, transcript
    finally:
        os.close(master)
        if slave is not None:
            os.close(slave)
