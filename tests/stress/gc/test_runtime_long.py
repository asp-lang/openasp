#!/usr/bin/env python3
# Copyright (c) 2026 https://asp-lang.org
# SPDX-License-Identifier: MIT

"""Run a duration-based FastCGI soak test with correctness and resource checks."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import time
import urllib.request

__import__("sys").path.insert(0, str(next(parent for parent in Path(__file__).resolve().parents if (parent / "Makefile").is_file()) / "tests/benchmarks/runners"))
from benchmark_memory import child_pids, darwin_physical_kib, rss_kib, transact_response, wait_for_workers


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, int((len(ordered) - 1) * fraction))
    return ordered[index]


def fd_counts(pids: list[int]) -> dict[int, int]:
    result: dict[int, int] = {}
    for pid in pids:
        completed = subprocess.run(
            ["lsof", "-a", "-p", str(pid), "-Fn"],
            check=False,
            capture_output=True,
            text=True,
        )
        if completed.returncode == 0:
            result[pid] = sum(1 for line in completed.stdout.splitlines() if line.startswith("f"))
    return result

def current_processes(server: subprocess.Popen[bytes], workers: int) -> tuple[int | None, list[int]]:
    children = child_pids(server.pid)
    if workers == 1 and not children:
        return None, [server.pid]
    return server.pid, children


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, default=Path("build/bin/openasp-fpm"))
    parser.add_argument("--root", type=Path, default=Path("tests/fixtures/common"))
    parser.add_argument("--duration-seconds", type=float, default=300)
    parser.add_argument("--concurrency", type=int, default=8)
    parser.add_argument("--workers", type=int, default=4)
    parser.add_argument("--iterations", type=int, default=4000)
    parser.add_argument("--sample-seconds", type=float, default=5)
    parser.add_argument("--max-requests", type=int, default=0)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.duration_seconds <= 0 or args.concurrency <= 0 or args.workers <= 0:
        parser.error("duration, concurrency, and workers must be positive")

    binary = args.binary.resolve()
    root = args.root.resolve()
    run_id = os.environ.get("DEBUG_RUN_ID", "pre-fix")
    stop = threading.Event()
    lock = threading.Lock()
    completed_requests = 0
    received_bytes = 0
    errors: list[str] = []
    latencies_ms: list[float] = []
    latency_seen = 0

    with tempfile.TemporaryDirectory(prefix="openasp-long-stress-") as temporary:
        temporary_path = Path(temporary)
        socket_path = temporary_path / "openasp.sock"
        command = [
            str(binary),
            "--root",
            str(root),
            "--unix-socket",
            str(socket_path),
            "--workers",
            str(args.workers),
            "--max-requests",
            str(args.max_requests),
            "--memory-idle-ms",
            "0",
            "--memory-relief-requests",
            "64",
            "--opcache-memory",
            "32",
            "--opcache-validate-timestamps=off",
            "--opcache-dir",
            str(temporary_path / "cache"),
            "--log-file",
            "stderr",
            "--log-level",
            "notice",
        ]
        stderr_path = temporary_path / "openasp-fpm.stderr"
        stderr_file = stderr_path.open("wb")
        server = subprocess.Popen(
            command,
            stdout=subprocess.DEVNULL,
            stderr=stderr_file,
            env=os.environ.copy(),
        )

        def request_worker() -> None:
            nonlocal completed_requests, latency_seen, received_bytes
            path = f"runtime_long_stress.asp?iterations={args.iterations}"
            while not stop.is_set():
                started = time.monotonic()
                try:
                    response = transact_response(socket_path, root, path)
                    if b"\r\n\r\nstress-ok|" not in response:
                        raise RuntimeError(f"invalid workload response: {response[-160:]!r}")
                    elapsed_ms = (time.monotonic() - started) * 1000
                    with lock:
                        completed_requests += 1
                        received_bytes += len(response)
                        latency_seen += 1
                        if len(latencies_ms) < 200000:
                            latencies_ms.append(elapsed_ms)
                        elif latency_seen % 1024 == 0:
                            latencies_ms[(latency_seen // 1024) % len(latencies_ms)] = elapsed_ms
                except Exception as error:
                    with lock:
                        if len(errors) < 100:
                            errors.append(repr(error))
                    stop.set()

        try:
            initial_worker_pids = wait_for_workers(server, socket_path, args.workers)
            worker_pid_changes: list[dict[str, object]] = []
            previous_worker_pids = initial_worker_pids
            started = time.monotonic()
            deadline = started + args.duration_seconds
            threads = [threading.Thread(target=request_worker) for _ in range(args.concurrency)]
            for thread in threads:
                thread.start()

            samples: list[dict[str, object]] = []
            previous_requests = 0
            previous_time = started
            next_sample = started
            while not stop.is_set() and time.monotonic() < deadline:
                now = time.monotonic()
                if now < next_sample:
                    time.sleep(min(0.1, next_sample - now))
                    continue
                master_pid, worker_pids = current_processes(server, args.workers)
                if worker_pids != previous_worker_pids:
                    worker_pid_changes.append({
                        "elapsed_seconds": round(now - started, 3),
                        "before": previous_worker_pids,
                        "after": worker_pids,
                    })
                    previous_worker_pids = worker_pids
                pids = ([master_pid] if master_pid is not None else []) + worker_pids
                with lock:
                    request_count = completed_requests
                    error_count = len(errors)
                rss = rss_kib(pids)
                physical = darwin_physical_kib(pids) if os.uname().sysname == "Darwin" else {}
                descriptors = fd_counts(pids)
                interval = max(now - previous_time, 0.001)
                sample = {
                    "elapsed_seconds": round(now - started, 3),
                    "requests": request_count,
                    "interval_rps": round((request_count - previous_requests) / interval, 3),
                    "errors": error_count,
                    "master_pid": master_pid,
                    "worker_pids": worker_pids,
                    "rss_kib": rss,
                    "physical_kib": physical,
                    "fd_count": descriptors,
                }
                samples.append(sample)
                previous_requests = request_count
                previous_time = now
                next_sample = now + args.sample_seconds
                if server.poll() is not None:
                    errors.append(f"FastCGI server exited with status {server.returncode}")
                    stop.set()

            stop.set()
            for thread in threads:
                thread.join(timeout=35)
            elapsed = time.monotonic() - started
            with lock:
                latency_copy = list(latencies_ms)
                final_requests = completed_requests
                final_errors = list(errors)

            initial_rss = sum(int(value) for value in samples[0]["rss_kib"].values()) if samples else 0
            final_rss = sum(int(value) for value in samples[-1]["rss_kib"].values()) if samples else 0
            peak_rss = max((sum(int(value) for value in sample["rss_kib"].values()) for sample in samples), default=0)
            result = {
                "run_id": run_id,
                "command": command,
                "duration_seconds": elapsed,
                "requests": final_requests,
                "requests_per_second": final_requests / elapsed if elapsed else 0,
                "errors": final_errors,
                "received_bytes": received_bytes,
                "initial_worker_pids": initial_worker_pids,
                "worker_pid_changes": worker_pid_changes,
                "latency_ms": {
                    "p50": percentile(latency_copy, 0.50),
                    "p95": percentile(latency_copy, 0.95),
                    "p99": percentile(latency_copy, 0.99),
                    "max": max(latency_copy, default=0),
                    "sample_count": len(latency_copy),
                },
                "rss_kib": {
                    "initial": initial_rss,
                    "final": final_rss,
                    "peak": peak_rss,
                    "growth": final_rss - initial_rss,
                },
                "samples": samples,
            }
            encoded = json.dumps(result, indent=2)
            if args.output:
                args.output.write_text(encoded + "\n", encoding="utf-8")
            print(encoded)
            return 1 if final_errors else 0
        finally:
            stop.set()
            if server.poll() is None:
                server.terminate()
                try:
                    server.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait(timeout=5)
            stderr_file.close()
            stderr = stderr_path.read_text(encoding="utf-8", errors="replace")
            if stderr:
                print(stderr[-8000:], file=os.sys.stderr)


if __name__ == "__main__":
    raise SystemExit(main())
