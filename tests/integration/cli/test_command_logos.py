#!/usr/bin/env python3
"""Verify that every OpenASP command prints the colored terminal logo."""

from __future__ import annotations

import errno
import fcntl
import os
from pathlib import Path
import pty
import re
import subprocess
import struct
import sys
import termios


ANSI = re.compile(rb"\x1b\[[0-9;]*m")


def source_version() -> bytes:
    version_header = Path(__file__).resolve().parents[3] / "src" / "cmd" / "version.h"
    match = re.search(r'^#define OPENASP_VERSION "([^"]+)"$', version_header.read_text(), re.MULTILINE)
    assert match, f"OPENASP_VERSION missing from {version_header}"
    return match.group(1).encode()


def run_in_terminal(command: list[str], columns: int = 100, rows: int = 24) -> bytes:
    master, slave = pty.openpty()
    size = struct.pack("HHHH", rows, columns, 0, 0)
    fcntl.ioctl(slave, termios.TIOCSWINSZ, size)
    environment = os.environ.copy()
    environment["TERM"] = "xterm-256color"
    environment.pop("NO_COLOR", None)
    process = subprocess.Popen(
        command,
        stdin=slave,
        stdout=slave,
        stderr=slave,
        env=environment,
        close_fds=True,
    )
    os.close(slave)
    output = bytearray()
    try:
        while True:
            try:
                chunk = os.read(master, 4096)
            except OSError as error:
                if error.errno == errno.EIO:
                    break
                raise
            if not chunk:
                break
            output.extend(chunk)
        process.wait(timeout=5)
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=5)
        os.close(master)
    return bytes(output)


def main() -> int:
    if len(sys.argv) != 6:
        raise SystemExit("usage: test_command_logos.py OPENASP OPENASP_FPM OPENASP_CLI OPENASP_DIS OPENASP_DBG")
    binaries = [Path(argument).resolve() for argument in sys.argv[1:]]
    logo_padding = b" " * ((100 - 59) // 2)
    copyright_text = b"Copyright (C) 2026 https://asp-lang.org"
    copyright_padding = b" " * ((100 - len(copyright_text) - 2) // 2)
    version_text = b"OpenASP " + source_version()
    for binary in binaries:
        output = run_in_terminal([str(binary), "--help"])
        assert b"\x1b[38;5;" in output, f"{binary.name}: colored logo missing: {output!r}"
        assert output.count(b"\x1b[0m") >= 6, f"{binary.name}: incomplete logo: {output!r}"
        assert version_text in output, f"{binary.name}: version missing: {output!r}"
        assert output.startswith(logo_padding + b"\x1b[38;5;45m"), (
            f"{binary.name}: logo is not centered: {output!r}"
        )
        copyright_line = (
            copyright_padding + b"\x1b[97;44m " + copyright_text + b" \x1b[0m\r\n"
        )
        assert copyright_line in output, f"{binary.name}: centered copyright style missing: {output!r}"

        for columns in (50, 20, 10):
            compact = run_in_terminal([str(binary), "--help"], columns=columns)
            assert b"\xe2\x96\x88" not in compact, f"{binary.name}: full logo used at {columns} columns"
            assert b"OPEN" in compact, f"{binary.name}: compact logo missing at {columns} columns"
            banner_lines = compact.split(b"\n")[:3]
            for line in banner_lines:
                visible = ANSI.sub(b"", line).rstrip(b"\r")
                assert len(visible) < columns, (
                    f"{binary.name}: banner wraps at {columns} columns: {visible!r}"
                )

        redirected = subprocess.run(
            [str(binary), "--help"],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        assert b"\x1b[" not in redirected.stdout + redirected.stderr, (
            f"{binary.name}: redirected output contains ANSI escapes"
        )
    print("command logo tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
