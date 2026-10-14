#!/usr/bin/env python3
# Copyright (c) 2026 https://asp-lang.org
# SPDX-License-Identifier: MIT

"""Monitor service workers created by OpenASP.Process inside an ASP controller."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time
import urllib.parse

__import__("sys").path.insert(0, str(next(parent for parent in Path(__file__).resolve().parents if (parent / "Makefile").is_file()) / "tests/benchmarks/runners"))
from benchmark_memory import darwin_physical_kib, rss_kib


GC_PATTERN = re.compile(
    r"OpenASP GC: heap=(?P<heap>\d+) external=(?P<external>\d+) "
    r"young=(?P<young>\d+) old=(?P<old>\d+) objects=(?P<objects>\d+) "
    r"minor=(?P<minor>\d+) major=(?P<major>\d+) promoted=(?P<promoted>\d+) "
    r"finalized=(?P<finalized>\d+) peak=(?P<peak>\d+) limit=(?P<limit>\d+) "
    r"allocation_failures=(?P<allocation_failures>\d+)"
)
WORKER_PATTERN = re.compile(
    r"^service-gc-ok\|(?P<iterations>\d+)\|(?P<checksum>\d+)\|"
    r"(?P<mutations>\d+)\|(?P<components>\d+)\|(?P<tail>\d+)$"
)
CONTROLLER_PATTERN = re.compile(
    r"^service-process-controller-ok\|(?P<processes>\d+)\|(?P<pids>\d+(?:,\d+)*)$"
)


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


def descendants(parent: int, table: dict[int, tuple[int, str]]) -> list[int]:
    result: list[int] = []
    frontier = [parent]
    while frontier:
        current = frontier.pop()
        children = [pid for pid, (ppid, _) in table.items() if ppid == current]
        result.extend(children)
        frontier.extend(children)
    return result


def fd_counts(pids: list[int]) -> dict[int, int]:
    counts: dict[int, int] = {}
    for pid in pids:
        completed = subprocess.run(
            ["lsof", "-a", "-p", str(pid), "-Fn"],
            check=False,
            capture_output=True,
            text=True,
        )
        if completed.returncode == 0:
            counts[pid] = sum(line.startswith("f") for line in completed.stdout.splitlines())
    return counts


def cpu_percentages(pids: list[int]) -> dict[int, float]:
    if not pids:
        return {}
    completed = subprocess.run(
        ["ps", "-o", "pid=,%cpu=", "-p", ",".join(map(str, pids))],
        check=False,
        capture_output=True,
        text=True,
    )
    values: dict[int, float] = {}
    for line in completed.stdout.splitlines():
        fields = line.split()
        if len(fields) == 2:
            values[int(fields[0])] = float(fields[1])
    return values


def parse_gc(text: str) -> dict[str, int] | None:
    matches = list(GC_PATTERN.finditer(text))
    return (
        {name: int(value) for name, value in matches[-1].groupdict().items()}
        if matches
        else None
    )


def metric_summary(samples: list[dict[str, object]], name: str, pid: int) -> dict[str, int | None]:
    values = [
        int(sample[name][str(pid)])
        for sample in samples
        if str(pid) in sample[name]
    ]
    return {
        "initial": values[0] if values else None,
        "final": values[-1] if values else None,
        "minimum": min(values, default=None),
        "maximum": max(values, default=None),
        "growth": values[-1] - values[0] if values else None,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, default=Path("build/bin/openasp"))
    parser.add_argument("--fixture", type=Path, default=Path("tests/fixtures/common/service_gc_megastress.asp"))
    parser.add_argument("--processes", type=int, required=True)
    parser.add_argument("--iterations", type=int, default=14_000_000)
    parser.add_argument("--sample-seconds", type=float, default=10)
    parser.add_argument("--timeout-seconds", type=float, default=1200)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.processes < 1 or args.processes > 32 or args.iterations < 1:
        parser.error("processes must be 1..32 and iterations must be positive")

    binary = args.binary.resolve()
    fixture = args.fixture.resolve()
    output = args.output.resolve()
    run_id = os.environ.get("DEBUG_RUN_ID", f"p{args.processes}")
    with tempfile.TemporaryDirectory(prefix=f"openasp-process-gc-p{args.processes}-") as raw:
        run_directory = Path(raw)
        query = urllib.parse.urlencode(
            {
                "role": "controller",
                "processes": args.processes,
                "iterations": args.iterations,
                "binary": binary,
                "script": fixture,
                "output": run_directory,
            }
        )
        command = [
            str(binary),
            str(fixture),
            "--root",
            str(fixture.parent),
            "--query",
            query,
            "--runtime-mode=service",
            "--gc-diagnostics",
            "--process-enable=on",
        ]
        environment = {
            **os.environ,
            "LC_ALL": "C",
            "DEBUG_RUN_ID": run_id,
        }
        started = time.monotonic()
        controller = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=environment,
        )
        deadline = started + args.timeout_seconds
        samples: list[dict[str, object]] = []
        observed_workers: set[int] = set()
        timed_out = False

        while controller.poll() is None:
            now = time.monotonic()
            if now >= deadline:
                timed_out = True
                controller.terminate()
                try:
                    controller.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    controller.kill()
                break
            table = process_table()
            children = descendants(controller.pid, table)
            workers = [
                pid
                for pid in children
                if pid in table and table[pid][0] == controller.pid
            ]
            observed_workers.update(workers)
            live_pids = [controller.pid, *children]
            rss = rss_kib(live_pids)
            physical = darwin_physical_kib(live_pids) if os.uname().sysname == "Darwin" else {}
            fds = fd_counts(live_pids)
            cpu = cpu_percentages(live_pids)
            samples.append(
                {
                    "elapsed_seconds": round(now - started, 3),
                    "controller_pid": controller.pid,
                    "descendant_pids": children,
                    "worker_pids": workers,
                    "states": {str(pid): table[pid][1] for pid in live_pids if pid in table},
                    "rss_kib": {str(pid): value for pid, value in rss.items()},
                    "physical_kib": {str(pid): value for pid, value in physical.items()},
                    "fd_count": {str(pid): value for pid, value in fds.items()},
                    "cpu_percent": {str(pid): value for pid, value in cpu.items()},
                }
            )
            time.sleep(args.sample_seconds)

        stdout_bytes, stderr_bytes = controller.communicate()
        elapsed = time.monotonic() - started
        controller_stdout = stdout_bytes.decode("utf-8", errors="replace").strip()
        controller_stderr = stderr_bytes.decode("utf-8", errors="replace")
        controller_match = CONTROLLER_PATTERN.fullmatch(controller_stdout)
        declared_pids = (
            [int(value) for value in controller_match.group("pids").split(",")]
            if controller_match
            else []
        )

        workers: list[dict[str, object]] = []
        errors: list[str] = []
        for index in range(args.processes):
            stdout_path = run_directory / f"worker-{index}.stdout"
            stderr_path = run_directory / f"worker-{index}.stderr"
            worker_stdout = stdout_path.read_text(errors="replace").strip() if stdout_path.exists() else ""
            worker_stderr = stderr_path.read_text(errors="replace") if stderr_path.exists() else ""
            match = WORKER_PATTERN.fullmatch(worker_stdout)
            gc = parse_gc(worker_stderr)
            if not match:
                errors.append(f"worker {index} output mismatch: {worker_stdout[:200]!r}")
            if not gc:
                errors.append(f"worker {index} has no final GC diagnostics")
            workers.append(
                {
                    "index": index,
                    "pid": declared_pids[index] if index < len(declared_pids) else None,
                    "result": (
                        {name: int(value) for name, value in match.groupdict().items()}
                        if match
                        else None
                    ),
                    "gc": gc,
                    "stderr_tail": worker_stderr[-1000:],
                }
            )

        if timed_out:
            errors.append("controller timed out")
        if controller.returncode != 0:
            errors.append(f"controller exit code {controller.returncode}")
        if not controller_match:
            errors.append(f"controller output mismatch: {controller_stdout[:300]!r}")
        if set(declared_pids) != observed_workers:
            errors.append(
                f"worker PID evidence differs: declared={declared_pids}, "
                f"observed={sorted(observed_workers)}"
            )
        zombie_samples = [
            sample["elapsed_seconds"]
            for sample in samples
            if any(str(state).startswith("Z") for state in sample["states"].values())
        ]
        if zombie_samples:
            errors.append(f"zombie processes observed at {zombie_samples[:10]}")

        report = {
            "processes": args.processes,
            "iterations_per_worker": args.iterations,
            "total_iterations": args.processes * args.iterations,
            "elapsed_seconds": round(elapsed, 6),
            "aggregate_iterations_per_second": round(
                args.processes * args.iterations / elapsed, 3
            ),
            "command": command,
            "controller": {
                "pid": controller.pid,
                "exit_code": controller.returncode,
                "stdout": controller_stdout,
                "gc": parse_gc(controller_stderr),
                "stderr_tail": controller_stderr[-1000:],
                "rss_kib": metric_summary(samples, "rss_kib", controller.pid),
                "physical_kib": metric_summary(samples, "physical_kib", controller.pid),
                "fd_count": metric_summary(samples, "fd_count", controller.pid),
            },
            "observed_worker_pids": sorted(observed_workers),
            "workers": workers,
            "samples": samples,
            "zombie_sample_count": len(zombie_samples),
            "errors": errors,
        }
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(report, indent=2) + "\n")
        print(
            f"processes={args.processes} elapsed={elapsed:.3f}s "
            f"workers={sorted(observed_workers)} errors={len(errors)}"
        )
        for error in errors:
            print(f"error: {error}")
        return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
