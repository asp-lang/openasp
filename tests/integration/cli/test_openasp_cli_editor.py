#!/usr/bin/env python3
"""Exercise openasp-cli terminal editing through a real pseudo-terminal."""

from __future__ import annotations

import os
from pathlib import Path
import pty
import select
import subprocess
import sys
import time


def read_until(master: int, expected: bytes, timeout: float = 5.0) -> bytes:
    output = bytearray()
    deadline = time.monotonic() + timeout
    while expected not in output:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise AssertionError(f"timed out waiting for {expected!r}: {bytes(output)!r}")
        readable, _, _ = select.select([master], [], [], remaining)
        if not readable:
            continue
        chunk = os.read(master, 4096)
        if not chunk:
            break
        output.extend(chunk)
    return bytes(output)


def main() -> int:
    executable = Path(sys.argv[1]).resolve()
    environment = os.environ.copy()
    environment["TERM"] = "xterm-256color"
    environment.pop("NO_COLOR", None)
    prompt = b"\x1b[1;38;5;45mOpenASP>\x1b[0m "
    master, slave = pty.openpty()
    process = subprocess.Popen(
        [str(executable)],
        stdin=slave,
        stdout=slave,
        stderr=slave,
        env=environment,
        close_fds=True,
    )
    os.close(slave)
    try:
        output = read_until(master, prompt)
        os.write(master, b"? 40 + 1\x1b[D\x1b[3~\x1b[D\x1b[C2\r")
        output += read_until(master, b"\r\n42\r\n" + prompt)
        assert b"\r\n42\r\n" in output, output
        os.write(master, b":quit\r")
        assert process.wait(timeout=5) == 0
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=5)
        os.close(master)
    print("openasp-cli line editor tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
