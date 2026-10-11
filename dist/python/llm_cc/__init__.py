"""Run the native llm-cc executable bundled with this platform wheel."""

import os
from pathlib import Path
import subprocess
import sys


def main():
    suffix = ".exe" if os.name == "nt" else ""
    binary = Path(__file__).resolve().parent / "bin" / f"llm-cc{suffix}"
    argv = [str(binary), *sys.argv[1:]]
    try:
        if os.name != "nt":
            os.execv(str(binary), argv)
        # Windows execv does not preserve the waiting console process.
        child = subprocess.Popen(argv)
        while True:
            try:
                return child.wait()
            except KeyboardInterrupt:
                # The console has already delivered Ctrl-C to the child.
                continue
    except OSError as error:
        print(f"llm-cc: {error}", file=sys.stderr)
        return 1
