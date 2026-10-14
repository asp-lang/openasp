#!/usr/bin/env python3
# Copyright (c) 2026 https://asp-lang.org
# SPDX-License-Identifier: MIT

"""Run concurrent long-lived OpenASP service runtimes and monitor each process."""

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
            counts[pid] = sum(1 for line in completed.stdout.splitlines() if line.startswith("f"))
    return counts


def cpu_percentages(pids: list[int]) -> dict[int, float]:
    if not pids:
        return {}
    completed = subprocess.run(
        ["ps", "-o", "pid=,%cpu=", "-p", ",".join(str(pid) for pid in pids)],
        check=False,
        capture_output=True,
        text=True,
    )
    percentages: dict[int, float] = {}
    if completed.returncode != 0:
        return percentages
    for line in completed.stdout.splitlines():
        fields = line.split()
        if len(fields) == 2:
            try:
                percentages[int(fields[0])] = float(fields[1])
            except ValueError:
                continue
    return percentages


def parse_gc(stderr: str) -> dict[str, int] | None:
    matches = list(GC_PATTERN.finditer(stderr))
    if not matches:
        return None
    return {name: int(value) for name, value in matches[-1].groupdict().items()}

def summarize_metric(samples: list[dict[str, object]], field: str, pid: int) -> dict[str, int | None]:
    values = [
        int(sample[field][str(pid)])
        for sample in samples
        if str(pid) in sample[field]
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
    parser.add_argument("--minimum-seconds", type=float, default=540)
    parser.add_argument("--timeout-seconds", type=float, default=900)
    parser.add_argument("--gc-old-threshold", default="128K")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if (
        args.processes <= 0
        or args.iterations <= 0
        or args.sample_seconds <= 0
        or args.timeout_seconds <= 0
    ):
        parser.error("processes, iterations, sample interval, and timeout must be positive")

    binary = args.binary.resolve()
    fixture = args.fixture.resolve()
    run_id = os.environ.get("DEBUG_RUN_ID", "pre-fix")
    usage_before = resource.getrusage(resource.RUSAGE_CHILDREN)

    with tempfile.TemporaryDirectory(prefix=f"openasp-service-gc-p{args.processes}-") as directory:
        temporary = Path(directory)
        processes: list[subprocess.Popen[bytes]] = []
        commands: list[list[str]] = []
        stdout_paths: list[Path] = []
        stderr_paths: list[Path] = []
        streams: list[tuple[object, object]] = []
        started = time.monotonic()
        for index in range(args.processes):
            worker_directory = temporary / f"worker-{index}"
            worker_directory.mkdir()
            query = urllib.parse.urlencode(
                {
                    "iterations": args.iterations,
                    "temp": str(worker_directory / "component-lifecycle.txt"),
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
            stdout_path = worker_directory / "stdout"
            stderr_path = worker_directory / "stderr"
            stdout_file = stdout_path.open("wb")
            stderr_file = stderr_path.open("wb")
            process = subprocess.Popen(
                command,
                stdout=stdout_file,
                stderr=stderr_file,
                env={**os.environ, "LC_ALL": "C"},
            )
            commands.append(command)
            stdout_paths.append(stdout_path)
            stderr_paths.append(stderr_path)
            streams.append((stdout_file, stderr_file))
            processes.append(process)

        launch_elapsed = time.monotonic() - started
        deadline = started + args.timeout_seconds
        completion_times: dict[int, float] = {}
        samples: list[dict[str, object]] = []
        timed_out = False

        while True:
            now = time.monotonic()
            live = [process for process in processes if process.poll() is None]
            for process in processes:
                if process.returncode is not None and process.pid not in completion_times:
                    completion_times[process.pid] = now - started
            if not live:
                break
            if now >= deadline:
                timed_out = True
                for process in live:
                    process.terminate()
                for process in live:
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=5)
                break

            pids = [process.pid for process in live]
            rss = rss_kib(pids)
            physical = darwin_physical_kib(pids) if os.uname().sysname == "Darwin" else {}
            descriptors = fd_counts(pids)
            cpu = cpu_percentages(pids)
            sample: dict[str, object] = {
                "elapsed_seconds": round(now - started, 3),
                "live_pids": pids,
                "rss_kib": {str(pid): value for pid, value in rss.items()},
                "physical_kib": {str(pid): value for pid, value in physical.items()},
                "fd_count": {str(pid): value for pid, value in descriptors.items()},
                "cpu_percent": {str(pid): value for pid, value in cpu.items()},
            }
            samples.append(sample)
            time.sleep(min(args.sample_seconds, max(0.01, deadline - time.monotonic())))

        for process in processes:
            process.wait()
            if process.pid not in completion_times:
                completion_times[process.pid] = time.monotonic() - started
        elapsed = time.monotonic() - started
        for stdout_file, stderr_file in streams:
            stdout_file.close()
            stderr_file.close()

        errors: list[str] = []
        workers: list[dict[str, object]] = []
        expected_prefix = f"service-gc-ok|{args.iterations}|"
        for index, process in enumerate(processes):
            stdout = stdout_paths[index].read_text(encoding="utf-8", errors="replace").strip()
            stderr = stderr_paths[index].read_text(encoding="utf-8", errors="replace")
            worker_errors: list[str] = []
            if process.returncode != 0:
                worker_errors.append(f"process exited with status {process.returncode}")
            if not stdout.startswith(expected_prefix):
                worker_errors.append(f"unexpected output: {stdout[-500:]!r}")
            duration = completion_times[process.pid]
            if duration < args.minimum_seconds:
                worker_errors.append(
                    f"runtime {duration:.3f}s was shorter than required {args.minimum_seconds:.3f}s"
                )
            gc_stats = parse_gc(stderr)
            if gc_stats is None:
                worker_errors.append("missing final GC diagnostics")
            errors.extend(f"pid {process.pid}: {error}" for error in worker_errors)
            workers.append(
                {
                    "index": index,
                    "pid": process.pid,
                    "returncode": process.returncode,
                    "elapsed_seconds": duration,
                    "iterations_per_second": args.iterations / duration if duration else 0,
                    "stdout": stdout,
                    "errors": worker_errors,
                    "gc": gc_stats,
                    "rss_kib": summarize_metric(samples, "rss_kib", process.pid),
                    "physical_kib": summarize_metric(samples, "physical_kib", process.pid),
                    "fd_count": summarize_metric(samples, "fd_count", process.pid),
                    "stderr_tail": stderr[-8000:],
                }
            )

        if timed_out:
            errors.append(f"run exceeded timeout of {args.timeout_seconds:.3f} seconds")
        usage_after = resource.getrusage(resource.RUSAGE_CHILDREN)
        result = {
            "run_id": run_id,
            "process_count": args.processes,
            "iterations_per_process": args.iterations,
            "total_iterations": args.iterations * args.processes,
            "launch_elapsed_seconds": launch_elapsed,
            "elapsed_seconds": elapsed,
            "aggregate_iterations_per_second": (
                args.iterations * args.processes / max(completion_times.values())
                if completion_times
                else 0
            ),
            "commands": commands,
            "timed_out": timed_out,
            "errors": errors,
            "workers": workers,
            "child_rusage": {
                "user_seconds": usage_after.ru_utime - usage_before.ru_utime,
                "system_seconds": usage_after.ru_stime - usage_before.ru_stime,
                "max_rss": usage_after.ru_maxrss,
                "minor_faults": usage_after.ru_minflt - usage_before.ru_minflt,
                "major_faults": usage_after.ru_majflt - usage_before.ru_majflt,
                "voluntary_context_switches": usage_after.ru_nvcsw - usage_before.ru_nvcsw,
                "involuntary_context_switches": usage_after.ru_nivcsw - usage_before.ru_nivcsw,
            },
            "samples": samples,
        }
        encoded = json.dumps(result, indent=2)
        if args.output:
            args.output.write_text(encoded + "\n", encoding="utf-8")
        print(encoded)
        return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
