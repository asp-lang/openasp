#!/usr/bin/env python3
# Copyright (c) 2026 https://asp-lang.org
# SPDX-License-Identifier: MIT

"""Drive and monitor the eight-worker Fork/TLV/Redis service workload."""

from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import re
import signal
import socket
import struct
import subprocess
import threading
import time
import urllib.parse

__import__("sys").path.insert(0, str(next(parent for parent in Path(__file__).resolve().parents if (parent / "Makefile").is_file()) / "tests/benchmarks/runners"))
from benchmark_memory import darwin_physical_kib, rss_kib
from test_service_gc import cpu_percentages, fd_counts, process_table


REQUEST = struct.Struct(">4sBBIII")
RESPONSE = struct.Struct(">4sBBIIIII")
MAGIC = b"O\x00AR"
GC_PATTERN = re.compile(
    r"OpenASP GC: heap=(?P<heap>\d+) external=(?P<external>\d+) "
    r"young=(?P<young>\d+) old=(?P<old>\d+) objects=(?P<objects>\d+) "
    r"minor=(?P<minor>\d+) major=(?P<major>\d+) promoted=(?P<promoted>\d+) "
    r"finalized=(?P<finalized>\d+) peak=(?P<peak>\d+) limit=(?P<limit>\d+) "
    r"allocation_failures=(?P<allocation_failures>\d+)"
)
WORKER_PATTERN = re.compile(
    r"fork-rpc-worker-ok\|(?P<worker>\d+)\|(?P<requests>\d+)\|"
    r"(?P<iterations>\d+)\|(?P<children>[0-9,]*)"
)


def receive_exact(client: socket.socket, length: int) -> bytes:
    output = bytearray()
    while len(output) < length:
        chunk = client.recv(length - len(output))
        if not chunk:
            raise RuntimeError(f"unexpected EOF after {len(output)} of {length} bytes")
        output.extend(chunk)
    return bytes(output)


def redis_command(*arguments: object) -> object:
    def encode_command(values: tuple[object, ...]) -> bytes:
        encoded = [str(value).encode() for value in values]
        request = bytearray(f"*{len(encoded)}\r\n".encode())
        for argument in encoded:
            request.extend(f"${len(argument)}\r\n".encode())
            request.extend(argument)
            request.extend(b"\r\n")
        return bytes(request)

    request = encode_command(("SELECT", 15)) + encode_command(arguments)
    with socket.create_connection(("127.0.0.1", 6379), timeout=5) as client:
        client.sendall(request)
        stream = client.makefile("rb")

        def read_value() -> object:
            marker = stream.read(1)
            line = stream.readline()
            if not marker or not line.endswith(b"\r\n"):
                raise RuntimeError("invalid Redis response")
            value = line[:-2]
            if marker == b"+":
                return value.decode(errors="replace")
            if marker == b":":
                return int(value)
            if marker == b"$":
                length = int(value)
                if length < 0:
                    return None
                data = stream.read(length)
                if stream.read(2) != b"\r\n":
                    raise RuntimeError("invalid Redis bulk suffix")
                return data.decode(errors="replace")
            if marker == b"*":
                length = int(value)
                return None if length < 0 else [read_value() for _ in range(length)]
            if marker == b"-":
                raise RuntimeError(value.decode(errors="replace"))
            raise RuntimeError(f"unsupported Redis marker {marker!r}")

        if read_value() != "OK":
            raise RuntimeError("cannot select Redis database 15")
        return read_value()


def redis_info(section: str) -> dict[str, str]:
    raw = redis_command("INFO", section)
    if not isinstance(raw, str):
        return {}
    result: dict[str, str] = {}
    for line in raw.splitlines():
        if ":" in line and not line.startswith("#"):
            key, value = line.split(":", 1)
            result[key] = value
    return result


def children_of(parent: int, table: dict[int, tuple[int, str]]) -> list[int]:
    return sorted(pid for pid, (ppid, _) in table.items() if ppid == parent)


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = max(0, min(len(ordered) - 1, math.ceil(len(ordered) * fraction) - 1))
    return ordered[index]


def expected_checksum(payload: bytes, request_id: int, batch_count: int, worker: int) -> int:
    checksum = (request_id * 17 + batch_count * 31) % 1_000_003
    for index, value in enumerate(payload, 1):
        checksum = (checksum * 33 + value + index) % 1_000_003
    for logical_index in range(1, batch_count + 1):
        checksum = (checksum * 17 + logical_index + worker + request_id) % 1_000_003
        checksum = (checksum ^ (checksum // 13)) & 0x7FFFFFFF
        checksum %= 1_000_003
    return checksum


def transact_open(client: socket.socket, opcode: int, request_id: int, batch_count: int, payload: bytes) -> tuple[int, int, int, int]:
    packet = REQUEST.pack(MAGIC, 1, opcode, request_id, batch_count, len(payload)) + payload
    client.sendall(packet)
    response = receive_exact(client, RESPONSE.size)
    magic, version, response_opcode, response_id, checksum, progress, worker, reply_items = RESPONSE.unpack(response)
    if magic != MAGIC or version != 1 or response_opcode != 129 or response_id != request_id:
        raise RuntimeError(f"invalid response header for request {request_id}")
    return checksum, progress, worker, reply_items


def transact(port: int, opcode: int, request_id: int, batch_count: int, payload: bytes) -> tuple[int, int, int, int]:
    with socket.create_connection(("127.0.0.1", port), timeout=15) as client:
        client.settimeout(15)
        return transact_open(client, opcode, request_id, batch_count, payload)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, default=Path("build/bin/openasp"))
    parser.add_argument("--fixture", type=Path, default=Path("tests/fixtures/common/fork_rpc_redis_service.asp"))
    parser.add_argument("--iterations", type=int, default=100_000_000)
    parser.add_argument("--batch-size", type=int, default=1000)
    parser.add_argument("--clients", type=int, default=8)
    parser.add_argument("--payload-bytes", type=int, default=256)
    parser.add_argument("--port", type=int, default=19091)
    parser.add_argument("--sample-seconds", type=float, default=5.0)
    parser.add_argument("--timeout-seconds", type=float, default=7200.0)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.iterations <= 0 or args.batch_size <= 0 or args.iterations % args.batch_size:
        parser.error("iterations must be positive and divisible by batch-size")

    binary = args.binary.resolve()
    fixture = args.fixture.resolve()
    output = args.output.resolve()
    run_id = os.environ.get("DEBUG_RUN_ID", f"rpc-{int(time.time())}")
    prefix = f"openasp:rpc:{run_id}"
    request_count = args.iterations // args.batch_size
    redis_command("SELECT", 15)
    redis_command("FLUSHDB")
    redis_before = {**redis_info("memory"), **redis_info("stats"), **redis_info("cpu")}
    query = urllib.parse.urlencode({"port": args.port, "prefix": prefix})
    command = [
        str(binary),
        str(fixture),
        "--root",
        str(fixture.parent),
        "--query",
        query,
        "--runtime-mode=service",
        "--gc-diagnostics",
        "--gc-old-threshold=128K",
        "--process-enable=on",
    ]
    environment = {
        **os.environ,
        "LC_ALL": "C",
    }
    started = time.monotonic()
    server = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=environment,
        start_new_session=True,
    )

    for _ in range(600):
        table = process_table()
        children = children_of(server.pid, table)
        try:
            workers = redis_command("SELECT", 15)
            workers = redis_command("HLEN", f"{prefix}:workers")
        except (OSError, RuntimeError):
            workers = 0
        if len(children) == 7 and workers == 8:
            break
        if server.poll() is not None:
            break
        time.sleep(0.1)
    else:
        workers = 0

    errors: list[str] = []
    if server.poll() is not None or workers != 8:
        errors.append(f"server startup failed: children={len(children)} workers={workers}")

    next_request = 1
    request_lock = threading.Lock()
    result_lock = threading.Lock()
    latencies_ms: list[float] = []
    milestones: dict[int, dict[str, object]] = {}
    worker_requests: dict[int, int] = {}
    response_bytes = 0
    request_bytes = 0
    completed_iterations = 0
    monitor_stop = threading.Event()
    samples: list[dict[str, object]] = []

    def client_worker() -> None:
        nonlocal next_request, response_bytes, request_bytes, completed_iterations
        try:
            with socket.create_connection(("127.0.0.1", args.port), timeout=15) as client:
                client.settimeout(15)
                while not errors:
                    with request_lock:
                        if next_request > request_count:
                            return
                        request_id = next_request
                        next_request += 1
                    payload = bytes((request_id + index) & 0xFF for index in range(args.payload_bytes))
                    request_started = time.monotonic()
                    checksum, progress, worker, reply_items = transact_open(
                        client, 1, request_id, args.batch_size, payload
                    )
                    if reply_items != 40:
                        raise RuntimeError(f"request {request_id}: reply item count {reply_items}")
                    expected = expected_checksum(payload, request_id, args.batch_size, worker)
                    if checksum != expected:
                        raise RuntimeError(
                            f"request {request_id}: checksum {checksum}, expected {expected}"
                        )
                    latency = (time.monotonic() - request_started) * 1000.0
                    with result_lock:
                        latencies_ms.append(latency)
                        request_bytes += REQUEST.size + len(payload)
                        response_bytes += RESPONSE.size
                        completed_iterations += args.batch_size
                        worker_requests[worker] = worker_requests.get(worker, 0) + 1
                        if progress % 1_000_000 == 0 and progress not in milestones:
                            milestone = {
                                "iterations": progress,
                                "elapsed_seconds": round(time.monotonic() - started, 3),
                                "worker": worker,
                                "request_id": request_id,
                                "throughput_iterations_per_second": round(
                                    progress / (time.monotonic() - started), 2
                                ),
                            }
                            milestones[progress] = milestone
                            print("MILESTONE " + json.dumps(milestone, sort_keys=True), flush=True)
        except (OSError, RuntimeError) as error:
            with result_lock:
                errors.append(str(error))

    def monitor() -> None:
        while not monitor_stop.wait(args.sample_seconds):
            table = process_table()
            pids = [server.pid, *children_of(server.pid, table)]
            if server.poll() is not None:
                return
            memory = redis_info("memory")
            stats = redis_info("stats")
            sample = {
                "elapsed_seconds": round(time.monotonic() - started, 3),
                "pids": pids,
                "states": {str(pid): table.get(pid, (0, "?"))[1] for pid in pids},
                "rss_kib": {str(pid): value for pid, value in rss_kib(pids).items()},
                "physical_kib": {
                    str(pid): value for pid, value in darwin_physical_kib(pids).items()
                },
                "fd_count": {str(pid): value for pid, value in fd_counts(pids).items()},
                "cpu_percent": {
                    str(pid): value for pid, value in cpu_percentages(pids).items()
                },
                "redis_used_memory": int(memory.get("used_memory", 0)),
                "redis_used_memory_rss": int(memory.get("used_memory_rss", 0)),
                "redis_total_commands": int(stats.get("total_commands_processed", 0)),
                "redis_rejected_connections": int(stats.get("rejected_connections", 0)),
                "completed_iterations": completed_iterations,
            }
            samples.append(sample)

    monitor_thread = threading.Thread(target=monitor, name="monitor", daemon=True)
    monitor_thread.start()
    clients = [
        threading.Thread(target=client_worker, name=f"client-{index}")
        for index in range(args.clients)
    ]
    for thread in clients:
        thread.start()
    deadline = started + args.timeout_seconds
    for thread in clients:
        remaining = deadline - time.monotonic()
        thread.join(max(0.0, remaining))
    if any(thread.is_alive() for thread in clients):
        errors.append("client timeout")

    for stop_id in range(1, 17):
        try:
            transact(args.port, 2, 0x70000000 + stop_id, 0, b"")
        except (OSError, RuntimeError):
            if server.poll() is not None:
                break
        time.sleep(0.05)
    try:
        server.wait(timeout=60)
    except subprocess.TimeoutExpired:
        errors.append("server did not stop cleanly")
        os.killpg(server.pid, signal.SIGTERM)
        try:
            server.wait(timeout=10)
        except subprocess.TimeoutExpired:
            os.killpg(server.pid, signal.SIGKILL)
            server.wait()
    monitor_stop.set()
    monitor_thread.join(timeout=10)
    stdout_bytes, stderr_bytes = server.communicate()
    elapsed = time.monotonic() - started
    stdout = stdout_bytes.decode(errors="replace")
    stderr = stderr_bytes.decode(errors="replace")
    worker_results = [
        {key: int(value) if key != "children" else value for key, value in match.groupdict().items()}
        for match in WORKER_PATTERN.finditer(stdout)
    ]
    gc_results = [
        {key: int(value) for key, value in match.groupdict().items()}
        for match in GC_PATTERN.finditer(stderr)
    ]
    redis_after = {**redis_info("memory"), **redis_info("stats"), **redis_info("cpu")}
    milestone_count = redis_command("SELECT", 15)
    milestone_count = redis_command("HLEN", f"{prefix}:milestones")
    redis_progress = redis_command("GET", f"{prefix}:progress")
    redis_progress = int(redis_progress or 0)
    zombie_samples = [
        sample["elapsed_seconds"]
        for sample in samples
        if any(str(state).startswith("Z") for state in sample["states"].values())
    ]
    runtime_zombie_samples = [
        sample["elapsed_seconds"]
        for sample in samples
        if int(sample["completed_iterations"]) < args.iterations
        and any(str(state).startswith("Z") for state in sample["states"].values())
    ]

    if server.returncode != 0:
        errors.append(f"server exit code {server.returncode}")
    if len(worker_results) != 8:
        errors.append(f"expected 8 worker results, got {len(worker_results)}")
    if len(gc_results) != 8:
        errors.append(f"expected 8 GC results, got {len(gc_results)}")
    if completed_iterations != args.iterations:
        errors.append(f"completed {completed_iterations} of {args.iterations} iterations")
    if sum(int(result["requests"]) for result in worker_results) != request_count:
        errors.append("worker RPC request totals do not match")
    if sum(int(result["iterations"]) for result in worker_results) != args.iterations:
        errors.append("worker iteration totals do not match")
    if sorted(int(result["worker"]) for result in worker_results) != list(range(8)):
        errors.append("worker indexes are incomplete or duplicated")
    if len(milestones) != args.iterations // 1_000_000:
        errors.append(f"observed {len(milestones)} client milestones")
    if milestone_count != args.iterations // 1_000_000:
        errors.append(f"Redis recorded {milestone_count} milestones")
    if redis_progress != args.iterations:
        errors.append(f"Redis progress is {redis_progress}, expected {args.iterations}")
    if runtime_zombie_samples:
        errors.append(f"zombie workers observed during load in {len(runtime_zombie_samples)} samples")

    report = {
        "run_id": run_id,
        "command": command,
        "returncode": server.returncode,
        "errors": errors,
        "iterations": args.iterations,
        "batch_size": args.batch_size,
        "rpc_requests": len(latencies_ms),
        "elapsed_seconds": round(elapsed, 3),
        "rpc_requests_per_second": round(len(latencies_ms) / elapsed, 2),
        "iterations_per_second": round(completed_iterations / elapsed, 2),
        "latency_ms": {
            "minimum": round(min(latencies_ms, default=0.0), 3),
            "p50": round(percentile(latencies_ms, 0.50), 3),
            "p95": round(percentile(latencies_ms, 0.95), 3),
            "p99": round(percentile(latencies_ms, 0.99), 3),
            "maximum": round(max(latencies_ms, default=0.0), 3),
        },
        "wire_bytes": {"request": request_bytes, "response": response_bytes},
        "worker_requests": worker_requests,
        "milestones": [milestones[key] for key in sorted(milestones)],
        "worker_results": worker_results,
        "gc_results": gc_results,
        "samples": samples,
        "redis_before": redis_before,
        "redis_after": redis_after,
        "redis_milestone_count": milestone_count,
        "redis_progress": redis_progress,
        "zombie_samples": zombie_samples,
        "runtime_zombie_samples": runtime_zombie_samples,
        "stdout": stdout,
        "stderr": stderr,
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2, sort_keys=True))
    print("FINAL " + json.dumps({key: report[key] for key in (
        "errors", "iterations", "rpc_requests", "elapsed_seconds",
        "rpc_requests_per_second", "iterations_per_second", "latency_ms",
        "wire_bytes", "worker_requests", "redis_milestone_count",
    )}, sort_keys=True))
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
