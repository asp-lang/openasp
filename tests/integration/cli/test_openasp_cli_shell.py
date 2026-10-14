#!/usr/bin/env python3
"""Exercise openasp-cli shell commands through pipes and a real terminal."""

from __future__ import annotations

import os
from pathlib import Path
import pty
import re
import select
import subprocess
import sys
import time


ANSI = re.compile(rb"\x1b\[[0-9;]*m")


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
        output.extend(os.read(master, 4096))
    return bytes(output)


def run_terminal(executable: Path, no_color: bool) -> tuple[bytes, int]:
    environment = os.environ.copy()
    environment["TERM"] = "xterm-256color"
    environment["SHELL"] = "/bin/sh"
    if no_color:
        environment["NO_COLOR"] = "1"
    else:
        environment.pop("NO_COLOR", None)

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
        prompt = (b"OpenASP> " if no_color
                  else b"\x1b[1;38;5;45mOpenASP>\x1b[0m ")
        output = read_until(master, prompt)
        os.write(master, b"!printf shell-output\r")
        output += read_until(master, b"shell-output" + prompt)
        os.write(master, b"!exit 7\r")
        output += read_until(master, prompt)
        os.write(master, b":quit\r")
        return output, process.wait(timeout=5)
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=5)
        os.close(master)


def main() -> int:
    executable = Path(sys.argv[1]).resolve()

    denied = subprocess.run(
        [str(executable), "--shell-enable=off"],
        input="!printf forbidden\n:quit\n",
        text=True,
        capture_output=True,
        check=False,
    )
    assert denied.returncode == 1, denied
    assert "forbidden" not in denied.stdout, denied
    assert "restart with --shell-enable=on" in denied.stderr, denied

    invalid = subprocess.run(
        [str(executable), "--shell-enable=yes"],
        text=True,
        capture_output=True,
        check=False,
    )
    assert invalid.returncode == 2, invalid
    assert "--shell-enable must be on or off" in invalid.stderr, invalid

    command = "!printf 'stdout-line\\n'; printf 'stderr-line\\n' >&2\n:quit\n"
    piped = subprocess.run(
        [str(executable)],
        input=command,
        text=True,
        capture_output=True,
        check=False,
    )
    assert piped.returncode == 0, piped
    assert piped.stdout == "stdout-line\n", piped
    assert "[shell]" not in piped.stdout, piped
    assert "stderr-line" in piped.stderr, piped
    assert not ANSI.search(piped.stdout.encode()), piped
    assert not ANSI.search(piped.stderr.encode()), piped

    colored, colored_status = run_terminal(executable, no_color=False)
    assert colored_status == 1, colored
    assert b"shell-output" in colored, colored
    assert b"\x1b[1;38;5;45mOpenASP>\x1b[0m " in colored, colored
    assert b"[shell]" not in colored, colored

    plain, plain_status = run_terminal(executable, no_color=True)
    assert plain_status == 1, plain
    assert b"shell-output" in plain, plain
    assert not ANSI.search(plain), plain

    print("openasp-cli shell command tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
