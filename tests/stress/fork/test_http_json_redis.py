#!/usr/bin/env python3
# Copyright (c) 2026 https://asp-lang.org
# SPDX-License-Identifier: MIT

"""Drive and monitor the eight-worker Fork/HTTP/JSON/Redis service workload."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import threading
import time
import urllib.parse

__import__("sys").path.insert(0, str(next(parent for parent in Path(__file__).resolve().parents if (parent / "Makefile").is_file()) / "tests/benchmarks/runners"))
__import__("sys").path.insert(0, str(next(parent for parent in Path(__file__).resolve().parents if (parent / "Makefile").is_file()) / "tests/support"))
from benchmark_memory import darwin_physical_kib, rss_kib
from test_rpc_redis import (
    GC_PATTERN,
    children_of,
    percentile,
    redis_command,
    redis_info,
)
from run_darwin_leaks import descendants
from test_service_gc import cpu_percentages, fd_counts, process_table


ENDPOINTS = (
    ("/kv/batch", 1, 8),
    ("/list/window", 2, 8),
    ("/zset/rank", 3, 16),
    ("/hash/profile", 4, 8),
    ("/mixed/transaction", 5, 40),
)
WORKER_PATTERN = re.compile(
    r"fork-http-json-worker-ok\|(?P<worker>\d+)\|(?P<requests>\d+)\|"
    r"(?P<iterations>\d+)\|(?P<children>[0-9,]*)"
)


class HttpConnection:
    """Minimal persistent HTTP/1.1 client with explicit framing checks."""

    def __init__(self, port: int, timeout: float) -> None:
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self.socket.settimeout(timeout)
        self.buffer = bytearray()

    def close(self) -> None:
        self.socket.close()

    def _receive_to(self, marker: bytes) -> bytes:
        while marker not in self.buffer:
            chunk = self.socket.recv(65536)
            if not chunk:
                raise RuntimeError("unexpected EOF while reading HTTP headers")
            self.buffer.extend(chunk)
        position = self.buffer.index(marker)
        result = bytes(self.buffer[:position])
        del self.buffer[: position + len(marker)]
        return result

    def _receive_exact(self, length: int) -> bytes:
        while len(self.buffer) < length:
            chunk = self.socket.recv(65536)
            if not chunk:
                raise RuntimeError(
                    f"unexpected EOF after {len(self.buffer)} of {length} HTTP body bytes"
                )
            self.buffer.extend(chunk)
        result = bytes(self.buffer[:length])
        del self.buffer[:length]
        return result

    def request(
        self,
        path: str,
        body: bytes,
        request_id: int,
        fragment_every: int,
    ) -> tuple[int, dict[str, str], bytes, int]:
        header = (
            f"POST {path} HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Content-Type: application/json\r\n"
            f"Content-Length: {len(body)}\r\n"
            "Connection: keep-alive\r\n\r\n"
        ).encode("ascii")
        packet = header + body
        if fragment_every > 0 and request_id % fragment_every == 0:
            first = min(19, len(header))
            second = min(len(packet), len(header) + max(1, len(body) // 3))
            self.socket.sendall(packet[:first])
            time.sleep(0.0001)
            self.socket.sendall(packet[first:second])
            time.sleep(0.0001)
            self.socket.sendall(packet[second:])
        else:
            self.socket.sendall(packet)

        raw_header = self._receive_to(b"\r\n\r\n")
        lines = raw_header.split(b"\r\n")
        status_fields = lines[0].decode("ascii", errors="strict").split(" ", 2)
        if len(status_fields) < 2 or status_fields[0] != "HTTP/1.1":
            raise RuntimeError(f"invalid HTTP status line {lines[0]!r}")
        status = int(status_fields[1])
        headers: dict[str, str] = {}
        for line in lines[1:]:
            if b":" not in line:
                raise RuntimeError(f"invalid HTTP header {line!r}")
            name, value = line.split(b":", 1)
            headers[name.decode("ascii").lower()] = value.decode("ascii").strip()
        content_length = int(headers.get("content-length", "-1"))
        if content_length < 0:
            raise RuntimeError("HTTP response omitted Content-Length")
        response_body = self._receive_exact(content_length)
        return status, headers, response_body, len(packet)


def expected_checksum(
    payload: str,
    request_id: int,
    batch_count: int,
    worker: int,
    endpoint_code: int,
) -> int:
    checksum = (
        request_id * 17
        + batch_count * 31
        + worker * 13
        + endpoint_code * 7
    ) % 1_000_003
    for index, value in enumerate(payload, 1):
        checksum = (checksum * 33 + ord(value) + index) % 1_000_003
    for logical_index in range(1, batch_count + 1):
        checksum = (
            checksum * 17
            + logical_index
            + request_id
            + worker
            + endpoint_code
        ) % 1_000_003
        checksum = (checksum ^ (checksum // 13)) & 0x7FFFFFFF
        checksum %= 1_000_003
    return checksum


def stop_worker(port: int, stop_id: int) -> None:
    connection = HttpConnection(port, 10)
    try:
        body = json.dumps({"stop": True, "id": stop_id}, separators=(",", ":")).encode()
        status, _, response, _ = connection.request("/admin/stop", body, stop_id, 0)
        decoded = json.loads(response)
        if status != 200 or decoded.get("ok") is not True:
            raise RuntimeError(f"invalid stop response {status}: {decoded!r}")
    finally:
        connection.close()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, default=Path("build/bin/openasp"))
    parser.add_argument(
        "--fixture",
        type=Path,
        default=Path("tests/fixtures/common/fork_http_json_redis_service.asp"),
    )
    parser.add_argument("--iterations", type=int, default=100_000_000)
    parser.add_argument("--batch-size", type=int, default=1000)
    parser.add_argument("--clients", type=int, default=8)
    parser.add_argument("--payload-bytes", type=int, default=512)
    parser.add_argument("--fragment-every", type=int, default=97)
    parser.add_argument("--port", type=int, default=19093)
    parser.add_argument("--sample-seconds", type=float, default=2.0)
    parser.add_argument("--timeout-seconds", type=float, default=7200.0)
    parser.add_argument("--leaks-at-exit", action="store_true")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if (
        args.iterations <= 0
        or args.batch_size <= 0
        or args.iterations % args.batch_size
        or args.clients <= 0
        or args.payload_bytes < 96
    ):
        parser.error(
            "iterations must be divisible by batch-size; clients must be positive; "
            "payload-bytes must be at least 96"
        )

    binary = args.binary.resolve()
    fixture = args.fixture.resolve()
    output = args.output.resolve()
    run_id = os.environ.get("DEBUG_RUN_ID", f"http-json-{int(time.time())}")
    prefix = f"openasp:http-json:{run_id}"
    request_count = args.iterations // args.batch_size
    redis_command("FLUSHDB")
    redis_before = {
        **redis_info("memory"),
        **redis_info("stats"),
        **redis_info("cpu"),
    }
    query = urllib.parse.urlencode({"port": args.port, "prefix": prefix})
    target_command = [
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
    command = (
        ["leaks", "--atExit", "--", *target_command]
        if args.leaks_at_exit
        else target_command
    )
    environment = {
        **os.environ,
        "LC_ALL": "C",
        "DEBUG_RUN_ID": run_id,
    }
    started = time.monotonic()
    server = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=environment,
        start_new_session=True,
    )

    children: list[int] = []
    workers: object = 0
    for _ in range(600):
        table = process_table()
        children = children_of(server.pid, table)
        try:
            workers = redis_command("HLEN", f"{prefix}:workers")
        except (OSError, RuntimeError):
            workers = 0
        expected_descendants = 1 if args.leaks_at_exit else 7
        if len(children) == expected_descendants and workers == 8:
            break
        if server.poll() is not None:
            break
        time.sleep(0.1)

    errors: list[str] = []
    if (
        server.poll() is not None
        or workers != 8
        or len(children) != expected_descendants
    ):
        errors.append(
            f"server startup failed: children={len(children)} workers={workers}"
        )

    next_request = 1
    request_lock = threading.Lock()
    result_lock = threading.Lock()
    latencies_ms: list[float] = []
    milestones: dict[int, dict[str, object]] = {}
    worker_requests: dict[int, int] = {}
    endpoint_requests = {path: 0 for path, _, _ in ENDPOINTS}
    request_bytes = 0
    response_bytes = 0
    completed_iterations = 0
    monitor_stop = threading.Event()
    samples: list[dict[str, object]] = []

    def client_worker() -> None:
        nonlocal next_request, request_bytes, response_bytes, completed_iterations
        connection: HttpConnection | None = None
        try:
            connection = HttpConnection(args.port, 30)
            while True:
                with request_lock:
                    if next_request > request_count:
                        return
                    request_id = next_request
                    next_request += 1
                path, endpoint_code, expected_items = ENDPOINTS[
                    (request_id - 1) % len(ENDPOINTS)
                ]
                payload = "".join(
                    chr(65 + ((request_id + index) % 26))
                    for index in range(args.payload_bytes)
                )
                request_object = {
                    "request_id": request_id,
                    "batch": args.batch_size,
                    "payload": payload,
                    "metadata": {
                        "endpoint": endpoint_code,
                        "sequence": request_id,
                        "tags": ["fork", "http", "json", "redis"],
                    },
                }
                body = json.dumps(
                    request_object, separators=(",", ":"), sort_keys=True
                ).encode("ascii")
                request_started = time.monotonic()
                status, headers, response, sent_bytes = connection.request(
                    path, body, request_id, args.fragment_every
                )
                latency = (time.monotonic() - request_started) * 1000.0
                if status != 200:
                    raise RuntimeError(
                        f"request {request_id}: HTTP {status}: {response[:200]!r}"
                    )
                if headers.get("content-type") != "application/json":
                    raise RuntimeError(
                        f"request {request_id}: invalid content type {headers!r}"
                    )
                decoded = json.loads(response)
                worker = int(decoded["worker"])
                if (
                    decoded.get("ok") is not True
                    or int(decoded["request_id"]) != request_id
                    or int(decoded["endpoint_code"]) != endpoint_code
                    or int(decoded["reply_items"]) != expected_items
                ):
                    raise RuntimeError(
                        f"request {request_id}: invalid response metadata {decoded!r}"
                    )
                expected = expected_checksum(
                    payload,
                    request_id,
                    args.batch_size,
                    worker,
                    endpoint_code,
                )
                if int(decoded["checksum"]) != expected:
                    raise RuntimeError(
                        f"request {request_id}: checksum {decoded['checksum']}, "
                        f"expected {expected}"
                    )
                items = decoded.get("items")
                if not isinstance(items, list) or len(items) not in (8, 16):
                    raise RuntimeError(
                        f"request {request_id}: invalid items payload {type(items)} "
                        f"length={len(items) if isinstance(items, list) else -1}"
                    )
                progress = int(decoded["progress"])
                with result_lock:
                    latencies_ms.append(latency)
                    request_bytes += sent_bytes
                    response_bytes += len(response)
                    completed_iterations += args.batch_size
                    worker_requests[worker] = worker_requests.get(worker, 0) + 1
                    endpoint_requests[path] += 1
                    if progress % 1_000_000 == 0 and progress not in milestones:
                        elapsed = time.monotonic() - started
                        milestone = {
                            "iterations": progress,
                            "elapsed_seconds": round(elapsed, 3),
                            "worker": worker,
                            "request_id": request_id,
                            "throughput_iterations_per_second": round(
                                progress / elapsed, 2
                            ),
                        }
                        milestones[progress] = milestone
                        print(
                            "MILESTONE "
                            + json.dumps(milestone, sort_keys=True),
                            flush=True,
                        )
        except (OSError, RuntimeError, ValueError, KeyError, json.JSONDecodeError) as error:
            with result_lock:
                errors.append(str(error))
        finally:
            if connection is not None:
                connection.close()

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
                "states": {
                    str(pid): table.get(pid, (0, "?"))[1] for pid in pids
                },
                "rss_kib": {
                    str(pid): value for pid, value in rss_kib(pids).items()
                },
                "physical_kib": {
                    str(pid): value
                    for pid, value in darwin_physical_kib(pids).items()
                },
                "fd_count": {
                    str(pid): value for pid, value in fd_counts(pids).items()
                },
                "cpu_percent": {
                    str(pid): value for pid, value in cpu_percentages(pids).items()
                },
                "redis_used_memory": int(memory.get("used_memory", 0)),
                "redis_used_memory_rss": int(memory.get("used_memory_rss", 0)),
                "redis_total_commands": int(
                    stats.get("total_commands_processed", 0)
                ),
                "redis_total_net_input_bytes": int(
                    stats.get("total_net_input_bytes", 0)
                ),
                "redis_total_net_output_bytes": int(
                    stats.get("total_net_output_bytes", 0)
                ),
                "redis_rejected_connections": int(
                    stats.get("rejected_connections", 0)
                ),
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
        thread.join(max(0.0, deadline - time.monotonic()))
    if any(thread.is_alive() for thread in clients):
        errors.append("client timeout")

    for stop_id in range(1, 17):
        try:
            stop_worker(args.port, 0x70000000 + stop_id)
        except (OSError, RuntimeError, ValueError, json.JSONDecodeError):
            if server.poll() is not None:
                break
        time.sleep(0.05)
    shutdown_deadline = time.monotonic() + 60
    observed_descendants: set[int] = set()
    while server.poll() is None and time.monotonic() < shutdown_deadline:
        if args.leaks_at_exit:
            table = process_table()
            observed_descendants.update(descendants(server.pid, table))
            for pid in observed_descendants:
                parent, state = table.get(pid, (-1, ""))
                if parent != server.pid and state.startswith("T"):
                    os.kill(pid, signal.SIGCONT)
        time.sleep(0.05)
    if args.leaks_at_exit:
        table = process_table()
        for pid in observed_descendants:
            _, state = table.get(pid, (-1, ""))
            if state.startswith("T"):
                os.kill(pid, signal.SIGCONT)
    if server.poll() is None:
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
        {
            key: int(value) if key != "children" else value
            for key, value in match.groupdict().items()
        }
        for match in WORKER_PATTERN.finditer(stdout)
    ]
    gc_results = [
        {key: int(value) for key, value in match.groupdict().items()}
        for match in GC_PATTERN.finditer(stderr)
    ]
    redis_after = {
        **redis_info("memory"),
        **redis_info("stats"),
        **redis_info("cpu"),
    }
    milestone_count = redis_command("HLEN", f"{prefix}:milestones")
    redis_progress = int(redis_command("GET", f"{prefix}:progress") or 0)
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
        errors.append(
            f"completed {completed_iterations} of {args.iterations} iterations"
        )
    if sum(int(result["requests"]) for result in worker_results) != request_count:
        errors.append("worker HTTP request totals do not match")
    if sum(int(result["iterations"]) for result in worker_results) != args.iterations:
        errors.append("worker iteration totals do not match")
    if sorted(int(result["worker"]) for result in worker_results) != list(range(8)):
        errors.append("worker indexes are incomplete or duplicated")
    expected_milestones = args.iterations // 1_000_000
    if len(milestones) != expected_milestones:
        errors.append(f"observed {len(milestones)} client milestones")
    if milestone_count != expected_milestones:
        errors.append(f"Redis recorded {milestone_count} milestones")
    if redis_progress != args.iterations:
        errors.append(
            f"Redis progress is {redis_progress}, expected {args.iterations}"
        )
    if runtime_zombie_samples:
        errors.append(
            f"zombie workers observed during load in {len(runtime_zombie_samples)} samples"
        )

    report = {
        "run_id": run_id,
        "command": command,
        "returncode": server.returncode,
        "errors": errors,
        "iterations": args.iterations,
        "batch_size": args.batch_size,
        "http_requests": len(latencies_ms),
        "elapsed_seconds": round(elapsed, 3),
        "http_requests_per_second": round(len(latencies_ms) / elapsed, 2),
        "iterations_per_second": round(completed_iterations / elapsed, 2),
        "latency_ms": {
            "minimum": round(min(latencies_ms, default=0.0), 3),
            "p50": round(percentile(latencies_ms, 0.50), 3),
            "p95": round(percentile(latencies_ms, 0.95), 3),
            "p99": round(percentile(latencies_ms, 0.99), 3),
            "maximum": round(max(latencies_ms, default=0.0), 3),
        },
        "wire_bytes": {
            "request": request_bytes,
            "response_body": response_bytes,
        },
        "fragment_every": args.fragment_every,
        "worker_requests": worker_requests,
        "endpoint_requests": endpoint_requests,
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
    final_keys = (
        "errors",
        "iterations",
        "http_requests",
        "elapsed_seconds",
        "http_requests_per_second",
        "iterations_per_second",
        "latency_ms",
        "wire_bytes",
        "worker_requests",
        "endpoint_requests",
        "redis_milestone_count",
    )
    print(
        "FINAL "
        + json.dumps({key: report[key] for key in final_keys}, sort_keys=True),
        flush=True,
    )
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
