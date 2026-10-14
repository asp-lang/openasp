#!/usr/bin/env python3
# Copyright (c) 2026 https://asp-lang.org
# SPDX-License-Identifier: MIT

"""Run macOS leaks --atExit and resume targets orphaned in a stopped state."""

from __future__ import annotations

import argparse
import os
import signal
import subprocess
import time


def process_table() -> dict[int, tuple[int, str]]:
    completed = subprocess.run(
        ["ps", "-axo", "pid=,ppid=,state="],
        check=False,
        capture_output=True,
        text=True,
    )
    table: dict[int, tuple[int, str]] = {}
    for line in completed.stdout.splitlines():
        fields = line.split()
        if len(fields) >= 3:
            table[int(fields[0])] = (int(fields[1]), fields[2])
    return table


def descendants(parent: int, table: dict[int, tuple[int, str]]) -> set[int]:
    found: set[int] = set()
    frontier = [parent]
    while frontier:
        current = frontier.pop()
        children = [pid for pid, (ppid, _) in table.items() if ppid == current]
        found.update(children)
        frontier.extend(children)
    return found


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("a command is required after --")

    leaks = subprocess.Popen(["leaks", "--atExit", "--", *command])
    observed: set[int] = set()
    deadline = time.monotonic() + args.timeout
    while leaks.poll() is None:
        table = process_table()
        observed.update(descendants(leaks.pid, table))
        for pid in observed:
            parent, state = table.get(pid, (-1, ""))
            if parent != leaks.pid and state.startswith("T"):
                os.kill(pid, signal.SIGCONT)
        if time.monotonic() >= deadline:
            leaks.terminate()
            try:
                leaks.wait(timeout=5)
            except subprocess.TimeoutExpired:
                leaks.kill()
            return 124
        time.sleep(0.05)

    table = process_table()
    for pid in observed:
        _, state = table.get(pid, (-1, ""))
        if state.startswith("T"):
            os.kill(pid, signal.SIGCONT)
    return leaks.returncode


if __name__ == "__main__":
    raise SystemExit(main())
