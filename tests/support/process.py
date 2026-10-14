#!/usr/bin/env python3
# Copyright (c) 2026 https://asp-lang.org
# SPDX-License-Identifier: MIT

"""Process lifecycle helpers for integration and end-to-end tests."""

from __future__ import annotations

import subprocess


def terminate(process: subprocess.Popen[bytes] | subprocess.Popen[str], timeout: float = 5.0) -> None:
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=timeout)
