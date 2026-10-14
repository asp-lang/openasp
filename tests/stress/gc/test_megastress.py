#!/usr/bin/env python3
# Copyright (c) 2026 https://asp-lang.org
# SPDX-License-Identifier: MIT

"""Run and monitor one long-lived OpenASP service-mode runtime."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import resource
import subprocess
import tempfile
import time
import urllib.parse
import urllib.request

__import__("sys").path.insert(0, str(next(parent for parent in Path(__file__).resolve().parents if (parent / "Makefile").is_file()) / "tests/benchmarks/runners"))
from benchmark_memory import darwin_physical_kib, rss_kib


GC_PATTERN = re.compile(
    r"OpenASP GC: heap=(?P<heap>\d+) external=(?P<external>\d+) "
    r"young=(?P<young>\d+) old=(?P<old>\d+) objects=(?P<objects>\d+) "
    r"minor=(?P<minor>\d+) major=(?P<major>\d+) promoted=(?P<promoted>\d+) "
    r"finalized=(?P<finalized>\d+) peak=(?P<peak>\d+) limit=(?P<limit>\d+) "
    r"allocation_failures=(?P<allocation_failures>\d+)"
)


def fd_count(pid: int) -> int | None:
    completed = subprocess.run(
        ["lsof", "-a", "-p", str(pid), "-Fn"],
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        return None
    return sum(1 for line in completed.stdout.splitlines() if line.startswith("f"))


def process_cpu(pid: int) -> tuple[float | None, str | None]:
    completed = subprocess.run(
        ["ps", "-p", str(pid), "-o", "%cpu=,time="],
        check=False,
        capture_output=True,
        text=True,
    )
    fields = completed.stdout.strip().split(maxsplit=1)
    if completed.returncode != 0 or len(fields) != 2:
        return None, None
    try:
        return float(fields[0]), fields[1]
    except ValueError:
        return None, fields[1]

def parse_gc(stderr: str) -> dict[str, int] | None:
    matches = list(GC_PATTERN.finditer(stderr))
    if not matches:
        return None
    return {name: int(value) for name, value in matches[-1].groupdict().items()}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, default=Path("build/bin/openasp"))
    parser.add_argument("--fixture", type=Path, default=Path("tests/fixtures/common/service_gc_megastress.asp"))
    parser.add_argument("--iterations", type=int, default=100_000_000)
    parser.add_argument("--sample-seconds", type=float, default=5)
    parser.add_argument("--minimum-seconds", type=float, default=0)
    parser.add_argument("--timeout-seconds", type=float, default=28_800)
    parser.add_argument("--gc-old-threshold", default="128K")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.iterations <= 0 or args.sample_seconds <= 0 or args.timeout_seconds <= 0:
        parser.error("iterations, sample interval, and timeout must be positive")

    binary = args.binary.resolve()
    fixture = args.fixture.resolve()
    run_id = os.environ.get("DEBUG_RUN_ID", "pre-fix")
    usage_before = resource.getrusage(resource.RUSAGE_CHILDREN)

    with tempfile.TemporaryDirectory(prefix="openasp-service-gc-") as directory:
        temporary = Path(directory)
        query = urllib.parse.urlencode(
            {
                "iterations": args.iterations,
                "temp": str(temporary / "component-lifecycle.txt"),
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
            f"--gc-old-threshold={args.gc_old_threshold}",
        ]
        stdout_path = temporary / "stdout"
        stderr_path = temporary / "stderr"
        with stdout_path.open("wb") as stdout_file, stderr_path.open("wb") as stderr_file:
            process = subprocess.Popen(
                command,
                stdout=stdout_file,
                stderr=stderr_file,
                env={**os.environ, "LC_ALL": "C"},
            )
            started = time.monotonic()
            deadline = started + args.timeout_seconds
            samples: list[dict[str, object]] = []
            timed_out = False
            while process.poll() is None:
                now = time.monotonic()
                if now >= deadline:
                    timed_out = True
                    process.terminate()
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=5)
                    break
                rss = rss_kib([process.pid]).get(process.pid)
                physical = (
                    darwin_physical_kib([process.pid]).get(process.pid)
                    if os.uname().sysname == "Darwin"
                    else None
                )
                cpu_percent, cpu_time = process_cpu(process.pid)
                sample = {
                    "elapsed_seconds": round(now - started, 3),
                    "rss_kib": rss,
                    "physical_kib": physical,
                    "fd_count": fd_count(process.pid),
                    "cpu_percent": cpu_percent,
                    "cpu_time": cpu_time,
                }
                samples.append(sample)
                time.sleep(min(args.sample_seconds, max(0.01, deadline - time.monotonic())))

            returncode = process.wait()
            elapsed = time.monotonic() - started

        stdout = stdout_path.read_text(encoding="utf-8", errors="replace").strip()
        stderr = stderr_path.read_text(encoding="utf-8", errors="replace")
        usage_after = resource.getrusage(resource.RUSAGE_CHILDREN)
        expected_prefix = f"service-gc-ok|{args.iterations}|"
        errors: list[str] = []
        if timed_out:
            errors.append(f"process exceeded timeout of {args.timeout_seconds:.3f} seconds")
        if returncode != 0:
            errors.append(f"openasp exited with status {returncode}")
        if not stdout.startswith(expected_prefix):
            errors.append(f"unexpected output: {stdout[-500:]!r}")
        if elapsed < args.minimum_seconds:
            errors.append(
                f"runtime {elapsed:.3f}s was shorter than required {args.minimum_seconds:.3f}s"
            )

        rss_values = [
            int(sample["rss_kib"])
            for sample in samples
            if sample["rss_kib"] is not None and int(sample["rss_kib"]) >= 1024
        ]
        physical_values = [
            int(sample["physical_kib"])
            for sample in samples
            if sample["physical_kib"] is not None and int(sample["physical_kib"]) >= 1024
        ]
        fd_values = [int(sample["fd_count"]) for sample in samples if sample["fd_count"] is not None]
        gc_stats = parse_gc(stderr)
        if gc_stats is None:
            errors.append("missing final GC diagnostics")
        result = {
            "run_id": run_id,
            "command": command,
            "pid": process.pid,
            "iterations": args.iterations,
            "elapsed_seconds": elapsed,
            "iterations_per_second": args.iterations / elapsed if elapsed else 0,
            "returncode": returncode,
            "stdout": stdout,
            "errors": errors,
            "gc": gc_stats,
            "rss_kib": {
                "initial": rss_values[0] if rss_values else None,
                "final": rss_values[-1] if rss_values else None,
                "minimum": min(rss_values, default=None),
                "maximum": max(rss_values, default=None),
                "growth": rss_values[-1] - rss_values[0] if rss_values else None,
            },
            "physical_kib": {
                "initial": physical_values[0] if physical_values else None,
                "final": physical_values[-1] if physical_values else None,
                "minimum": min(physical_values, default=None),
                "maximum": max(physical_values, default=None),
                "growth": physical_values[-1] - physical_values[0] if physical_values else None,
            },
            "fd_count": {
                "initial": fd_values[0] if fd_values else None,
                "final": fd_values[-1] if fd_values else None,
                "minimum": min(fd_values, default=None),
                "maximum": max(fd_values, default=None),
            },
            "child_rusage": {
                "user_seconds": usage_after.ru_utime - usage_before.ru_utime,
                "system_seconds": usage_after.ru_stime - usage_before.ru_stime,
                "max_rss": usage_after.ru_maxrss,
                "minor_faults": usage_after.ru_minflt - usage_before.ru_minflt,
                "major_faults": usage_after.ru_majflt - usage_before.ru_majflt,
                "voluntary_context_switches": usage_after.ru_nvcsw - usage_before.ru_nvcsw,
                "involuntary_context_switches": usage_after.ru_nivcsw - usage_before.ru_nivcsw,
            },
            "stderr_tail": stderr[-8000:],
            "samples": samples,
        }
        encoded = json.dumps(result, indent=2)
        if args.output:
            args.output.write_text(encoded + "\n", encoding="utf-8")
        print(encoded)
        return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
