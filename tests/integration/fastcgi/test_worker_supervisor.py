#!/usr/bin/env python3
# Copyright (c) 2026 https://asp-lang.org
# SPDX-License-Identifier: MIT

"""Verify that the FastCGI master replaces a failed worker in place."""

from __future__ import annotations

from pathlib import Path
import os
import signal
import subprocess
import sys
import tempfile
import time

from test_fastcgi import start_server, transact


def child_processes(parent: int) -> list[int]:
    output = subprocess.check_output(
        ["ps", "-axo", "pid=,ppid="], text=True
    )
    return sorted(
        int(pid)
        for line in output.splitlines()
        for pid, ppid in [line.split()]
        if int(ppid) == parent
    )


def main() -> int:
    binary = Path(sys.argv[1]).resolve()
    fixtures = Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(prefix="openasp-supervisor-") as temporary:
        socket_path = Path(temporary) / "openasp.sock"
        command = [
            str(binary),
            "--root",
            str(fixtures),
            "--unix-socket",
            str(socket_path),
            "--workers",
            "2",
            "--opcache-dir",
            str(Path(temporary) / "cache"),
        ]
        server = start_server(command, socket_path)
        try:
            before = child_processes(server.pid)
            assert len(before) == 2, before
            os.kill(before[0], signal.SIGKILL)

            deadline = time.monotonic() + 5
            after = before
            while time.monotonic() < deadline:
                time.sleep(0.05)
                after = child_processes(server.pid)
                if len(after) == 2 and before[0] not in after:
                    break

            assert server.poll() is None, "FastCGI master exited with its worker"
            assert len(after) == 2 and before[0] not in after, (before, after)

            params = {
                "SCRIPT_FILENAME": str(fixtures / "index.asp"),
                "SCRIPT_NAME": "/index.asp",
                "REQUEST_METHOD": "GET",
                "QUERY_STRING": "name=Supervisor",
                "SERVER_NAME": "localhost",
                "SERVER_PORT": "80",
            }
            response = transact(socket_path, params)
            assert b"Status: 200 OK" in response, response
            assert b"<div>hello=Supervisor</div>" in response, response
            return 0
        finally:
            if server.poll() is None:
                server.terminate()
                server.wait(timeout=5)


if __name__ == "__main__":
    raise SystemExit(main())
