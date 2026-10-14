#!/usr/bin/env python3
# Copyright (c) 2026 https://asp-lang.org
# SPDX-License-Identifier: MIT

"""Shared FastCGI protocol helpers."""

from __future__ import annotations

from pathlib import Path
import socket
import subprocess
import time


def wait_for_unix_socket(process: subprocess.Popen[bytes], socket_path: Path, timeout: float = 5.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"server exited with status {process.returncode}")
        if socket_path.exists():
            try:
                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as probe:
                    probe.connect(str(socket_path))
                return
            except OSError:
                pass
        time.sleep(0.02)
    raise TimeoutError(f"server socket was not ready: {socket_path}")
