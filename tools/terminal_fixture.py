"""Run a command with redirected stdin and a separate controlling terminal."""

from contextlib import ExitStack
import fcntl
from functools import partial
import os
import pty
import select
import subprocess
import termios
import time


PROMPT = b"Download? [y/N] "


def attach_terminal(slave):
    os.setsid()
    fcntl.ioctl(slave, termios.TIOCSCTTY, 0)


def read_terminal(master, remaining):
    if select.select([master], [], [], min(remaining, 0.1))[0]:
        return os.read(master, 4096)
    return b""


def answer_prompts(process, master, responses, timeout):
    transcript = b""
    sent = 0
    deadline = time.monotonic() + timeout
    while process.poll() is None:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError(f"prompt hung after {sent} answers: {transcript!r}")
        transcript += read_terminal(master, remaining)
        if transcript.count(PROMPT) > sent:
            # Extra prompts fail immediately when no answer was expected.
            os.write(master, responses[sent])
            sent += 1
    if sent != len(responses):
        raise AssertionError(f"sent {sent}/{len(responses)} answers: {transcript!r}")
    return transcript


def run_terminal(command, responses, *, source=b"", env=None, timeout=5):
    with ExitStack() as cleanup:
        master, slave = pty.openpty()
        cleanup.callback(os.close, master)
        # Keep the parent slave open until exit so EOF cannot race with the
        # prompt reader. Child exit is observed through process.poll().
        cleanup.callback(os.close, slave)
        process = cleanup.enter_context(subprocess.Popen(
            command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, env=env, pass_fds=(slave,),
            preexec_fn=partial(attach_terminal, slave),
        ))
        # Popen.kill() tolerates an exited child; its context manager reaps it.
        cleanup.callback(process.kill)
        process.stdin.write(source)
        process.stdin.close()
        process.stdin = None
        transcript = answer_prompts(process, master, responses, timeout)
        stdout, stderr = process.communicate(timeout=1)
        return process.returncode, stdout, stderr, transcript
