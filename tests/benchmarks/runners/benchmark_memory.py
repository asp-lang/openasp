#!/usr/bin/env python3
# Copyright (c) 2026 https://asp-lang.org
# SPDX-License-Identifier: MIT

"""Measure per-process RSS while exercising an OpenASP FastCGI worker pool."""

from __future__ import annotations

import argparse
import ctypes
import json
import math
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time


def run_native_load(load_generator: Path, socket_path: Path, root: Path, paths: list[str], requests: int,
                    concurrency: int, cookie: str, pids: list[int], samples: list[dict[str, object]],
                    sample_interval: float) -> dict[str, object]:
    command = [
        str(load_generator), "--socket", str(socket_path), "--root", str(root), "--cookie", cookie,
        "--requests", str(requests), "--concurrency", str(concurrency),
        *[option for path in paths for option in ("--path", path)],
    ]
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    started = time.monotonic()
    while process.poll() is None:
        samples.append(memory_sample(pids, elapsed=time.monotonic() - started))
        time.sleep(sample_interval)
    stdout, stderr = process.communicate()
    if process.returncode:
        raise RuntimeError(f"native FastCGI load failed ({process.returncode}): {stderr.strip()}")
    try:
        result = json.loads(stdout)
    except json.JSONDecodeError as error:
        raise RuntimeError(f"native FastCGI load produced invalid JSON: {stdout[-1000:]}") from error
    if int(result.get("successful_requests", 0)) != requests:
        raise RuntimeError(f"native FastCGI load completed {result.get('successful_requests')} of {requests} requests")
    return result


def record(kind: int, request_id: int, content: bytes) -> bytes:
    return struct.pack("!BBHHBB", 1, kind, request_id, len(content), 0, 0) + content


def parameter(name: str, value: str) -> bytes:
    name_bytes = name.encode()
    value_bytes = value.encode()

    def encode_length(length: int) -> bytes:
        return bytes([length]) if length < 128 else struct.pack("!I", length | 0x80000000)

    return encode_length(len(name_bytes)) + encode_length(len(value_bytes)) + name_bytes + value_bytes


def receive_exact(client: socket.socket, length: int) -> bytes:
    output = bytearray()
    while len(output) < length:
        chunk = client.recv(length - len(output))
        if not chunk:
            raise RuntimeError("unexpected FastCGI EOF")
        output.extend(chunk)
    return bytes(output)


def transact_response(socket_path: Path, root: Path, relative_path: str, query: str = "", cookie: str = "") -> bytes:
    if "?" in relative_path:
        relative_path, path_query = relative_path.split("?", 1)
        if not query:
            query = path_query
    params = {
        "SCRIPT_FILENAME": str(root / relative_path),
        "SCRIPT_NAME": "/" + relative_path,
        "REQUEST_METHOD": "GET",
        "QUERY_STRING": query,
        "REQUEST_URI": "/" + relative_path + (("?" + query) if query else ""),
        "DOCUMENT_ROOT": str(root),
        "SERVER_PROTOCOL": "HTTP/1.1",
        "GATEWAY_INTERFACE": "CGI/1.1",
        "SERVER_SOFTWARE": "openasp-memory-benchmark",
        "SERVER_NAME": "localhost",
        "SERVER_PORT": "80",
        "REDIRECT_STATUS": "200",
    }
    if cookie:
        params["HTTP_COOKIE"] = cookie
    encoded = b"".join(parameter(name, value) for name, value in params.items())
    payload = (
        record(1, 1, struct.pack("!HB5x", 1, 0))
        + record(4, 1, encoded)
        + record(4, 1, b"")
        + record(5, 1, b"")
    )
    output = bytearray()
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(30)
        client.connect(str(socket_path))
        client.sendall(payload)
        while True:
            header = receive_exact(client, 8)
            _, kind, request_id, content_length, padding, _ = struct.unpack("!BBHHBB", header)
            if request_id != 1:
                raise RuntimeError(f"unexpected request id {request_id}")
            content = receive_exact(client, content_length)
            if padding:
                receive_exact(client, padding)
            if kind == 6:
                output.extend(content)
            if kind == 3:
                break
    headers = bytes(output).split(b"\r\n\r\n", 1)[0].splitlines()
    status_headers = [line for line in headers if line.lower().startswith(b"status:")]
    status_ok = any(line.lower().startswith(b"status: 2") for line in status_headers)
    if not status_headers:
        status_ok = any(line.lower().startswith(b"content-type:") for line in headers)
    if not status_ok:
        raise RuntimeError(f"{relative_path} did not return status 200")
    return bytes(output)


def transact(socket_path: Path, root: Path, relative_path: str, query: str = "", cookie: str = "") -> int:
    return len(transact_response(socket_path, root, relative_path, query, cookie))


def child_pids(parent: int) -> list[int]:
    result = subprocess.run(["pgrep", "-P", str(parent)], check=False, capture_output=True, text=True)
    return sorted(int(value) for value in result.stdout.split() if value.isdigit())


class ProcTaskInfo(ctypes.Structure):
    _fields_ = [
        ("virtual_size", ctypes.c_uint64),
        ("resident_size", ctypes.c_uint64),
        ("total_user", ctypes.c_uint64),
        ("total_system", ctypes.c_uint64),
        ("threads_user", ctypes.c_uint64),
        ("threads_system", ctypes.c_uint64),
        ("policy", ctypes.c_int32),
        ("faults", ctypes.c_int32),
        ("pageins", ctypes.c_int32),
        ("cow_faults", ctypes.c_int32),
        ("messages_sent", ctypes.c_int32),
        ("messages_received", ctypes.c_int32),
        ("syscalls_mach", ctypes.c_int32),
        ("syscalls_unix", ctypes.c_int32),
        ("context_switches", ctypes.c_int32),
        ("thread_count", ctypes.c_int32),
        ("running_thread_count", ctypes.c_int32),
        ("priority", ctypes.c_int32),
    ]


class ProcRusageInfoV2(ctypes.Structure):
    _fields_ = [
        ("uuid", ctypes.c_uint8 * 16),
        *[(name, ctypes.c_uint64) for name in (
            "user_time", "system_time", "package_idle_wakeups", "interrupt_wakeups", "pageins",
            "wired_size", "resident_size", "physical_footprint", "process_start", "process_exit",
            "child_user_time", "child_system_time", "child_package_idle_wakeups", "child_interrupt_wakeups",
            "child_pageins", "child_elapsed", "disk_bytes_read", "disk_bytes_written",
        )],
    ]


def darwin_rss_kib(pids: list[int]) -> dict[int, int]:
    libproc = ctypes.CDLL("/usr/lib/libproc.dylib", use_errno=True)
    proc_pidinfo = libproc.proc_pidinfo
    proc_pidinfo.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_int]
    proc_pidinfo.restype = ctypes.c_int
    result: dict[int, int] = {}
    for pid in pids:
        info = ProcTaskInfo()
        size = proc_pidinfo(pid, 4, 0, ctypes.byref(info), ctypes.sizeof(info))
        if size == ctypes.sizeof(info):
            result[pid] = int(info.resident_size // 1024)
    return result


def darwin_physical_kib(pids: list[int]) -> dict[int, int]:
    libproc = ctypes.CDLL("/usr/lib/libproc.dylib", use_errno=True)
    proc_pid_rusage = libproc.proc_pid_rusage
    proc_pid_rusage.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_void_p]
    proc_pid_rusage.restype = ctypes.c_int
    result: dict[int, int] = {}
    for pid in pids:
        info = ProcRusageInfoV2()
        if proc_pid_rusage(pid, 2, ctypes.byref(info)) == 0:
            result[pid] = int(info.physical_footprint // 1024)
    return result


def rss_kib(pids: list[int]) -> dict[int, int]:
    if not pids:
        return {}
    if sys.platform == "darwin":
        return darwin_rss_kib(pids)
    result = subprocess.run(
        ["ps", "-o", "pid=,rss=", "-p", ",".join(str(pid) for pid in pids)],
        check=True,
        capture_output=True,
        text=True,
    )
    return {int(pid): int(rss) for pid, rss in (line.split() for line in result.stdout.splitlines())}


def memory_sample(pids: list[int], **fields: object) -> dict[str, object]:
    sample = {**fields, "rss_kib": rss_kib(pids)}
    if sys.platform == "darwin":
        sample["physical_kib"] = darwin_physical_kib(pids)
    return sample


def percentile(values: list[int], fraction: float) -> int:
    if not values:
        return 0
    ordered = sorted(values)
    return ordered[max(0, min(len(ordered) - 1, math.ceil(len(ordered) * fraction) - 1))]


def summarize_rss(rss: dict[int, int], master_pid: int | None, worker_pids: list[int]) -> dict[str, int]:
    workers = [rss.get(pid, 0) for pid in worker_pids]
    worker_total = sum(workers)
    return {
        "total_kib": worker_total + (rss.get(master_pid, 0) if master_pid is not None else 0),
        "master_kib": rss.get(master_pid, 0) if master_pid is not None else 0,
        "worker_total_kib": worker_total,
        "worker_min_kib": min(workers, default=0),
        "worker_median_kib": percentile(workers, 0.5),
        "worker_p95_kib": percentile(workers, 0.95),
        "worker_max_kib": max(workers, default=0),
    }


def wait_for_workers(server: subprocess.Popen[bytes], socket_path: Path, workers: int) -> list[int]:
    for _ in range(500):
        pids = child_pids(server.pid)
        if socket_path.exists() and workers == 1 and not pids:
            return [server.pid]
        if socket_path.exists() and len(pids) == workers:
            return pids
        if server.poll() is not None:
            raise RuntimeError("FastCGI server exited before becoming ready")
        time.sleep(0.01)
    raise RuntimeError("FastCGI worker pool did not become ready")


def run_load(socket_path: Path, root: Path, paths: list[str], requests: int, concurrency: int, cookie: str,
             cookie_mode: str, pids: list[int], samples: list[dict[str, object]]) -> dict[str, object]:
    lock = threading.Lock()
    next_request = 0
    errors: list[str] = []
    bytes_received = 0

    def worker(thread_index: int) -> None:
        nonlocal next_request, bytes_received
        thread_cookie = cookie
        if cookie_mode == "none":
            thread_cookie = ""
        elif cookie_mode == "thread":
            thread_cookie = f"{cookie}-{thread_index}"
        while True:
            with lock:
                index = next_request
                next_request += 1
            if index >= requests:
                return
            try:
                size = transact(socket_path, root, paths[index % len(paths)], cookie=thread_cookie)
                with lock:
                    bytes_received += size
            except Exception as error:
                with lock:
                    errors.append(str(error))

    threads = [threading.Thread(target=worker, args=(index,)) for index in range(concurrency)]
    started = time.monotonic()
    for thread in threads:
        thread.start()
    while any(thread.is_alive() for thread in threads):
        samples.append(memory_sample(pids, elapsed=time.monotonic() - started))
        time.sleep(0.05)
    for thread in threads:
        thread.join()
    elapsed = time.monotonic() - started
    if errors:
        raise RuntimeError(f"{len(errors)} requests failed; first error: {errors[0]}")
    return {
        "requests": requests,
        "concurrency": concurrency,
        "cookie_mode": cookie_mode,
        "elapsed_seconds": elapsed,
        "requests_per_second": requests / elapsed,
        "bytes_received": bytes_received,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, default=Path("build/bin/openasp-fpm"))
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--workers", type=int, default=1)
    parser.add_argument("--opcache-memory", type=int, default=16)
    parser.add_argument("--opcache-validate-timestamps", choices=("on", "off"), default="off")
    parser.add_argument("--max-requests", type=int, default=0)
    parser.add_argument("--memory-relief-requests", type=int, default=64)
    parser.add_argument("--memory-limit", default="128M")
    parser.add_argument("--requests", type=int, default=1000)
    parser.add_argument("--concurrency", type=int, default=8)
    parser.add_argument("--sample-interval", type=float, default=0.25)
    parser.add_argument("--hold-seconds", type=float, default=0)
    parser.add_argument("--cookie", default="OPENASPID=memory-benchmark")
    parser.add_argument("--cookie-mode", choices=("shared", "thread", "none"), default="shared")
    parser.add_argument("--paths", nargs="+", default=["default.asp"])
    parser.add_argument("--load-generator", type=Path, default=Path("build/tests/bin/fastcgi-load"))
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    binary = args.binary.resolve()
    root = args.root.resolve()
    load_generator = args.load_generator.resolve()
    if not load_generator.is_file():
        raise RuntimeError(f"native FastCGI load generator does not exist: {load_generator}")
    if args.sample_interval <= 0:
        raise RuntimeError("sample interval must be positive")
    with tempfile.TemporaryDirectory(prefix="openasp-memory-") as temporary:
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
            "--opcache-memory",
            str(args.opcache_memory),
            f"--opcache-validate-timestamps={args.opcache_validate_timestamps}",
            "--max-requests",
            str(args.max_requests),
            "--memory-relief-requests",
            str(args.memory_relief_requests),
            "--memory-limit",
            args.memory_limit,
            "--opcache-dir",
            str(temporary_path / "cache"),
        ]
        log_path = temporary_path / "openasp-fpm.stderr"
        log_file = log_path.open("wb")
        server = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=log_file, env=os.environ.copy())
        try:
            workers = wait_for_workers(server, socket_path, args.workers)
            master_pid = None if args.workers == 1 else server.pid
            pids = workers if master_pid is None else [master_pid, *workers]
            samples: list[dict[str, object]] = [memory_sample(pids, phase="idle")]
            cold = run_load(socket_path, root, args.paths, len(args.paths), 1, args.cookie, args.cookie_mode,
                            pids, samples)
            samples.append(memory_sample(pids, phase="cold"))
            if args.cookie_mode == "none":
                raise RuntimeError("native load generator requires a non-empty cookie mode")
            hot = run_native_load(load_generator, socket_path, root, args.paths, args.requests, args.concurrency,
                                  args.cookie, pids, samples, args.sample_interval)
            samples.append(memory_sample(pids, phase="hot"))
            if args.hold_seconds > 0:
                time.sleep(args.hold_seconds)
            peak = {pid: max(int(sample["rss_kib"].get(pid, 0)) for sample in samples) for pid in pids}
            phase_samples = {str(sample["phase"]): sample for sample in samples if "phase" in sample}
            peak_sample = max(samples, key=lambda sample: sum(int(value) for value in sample["rss_kib"].values()))
            rss_summary = {
                phase: summarize_rss(sample["rss_kib"], master_pid, workers)
                for phase, sample in phase_samples.items()
            }
            rss_summary["synchronous_peak"] = summarize_rss(peak_sample["rss_kib"], master_pid, workers)
            physical_summary = None
            if all("physical_kib" in sample for sample in samples):
                peak_physical_sample = max(
                    samples, key=lambda sample: sum(int(value) for value in sample["physical_kib"].values())
                )
                physical_summary = {
                    phase: summarize_rss(sample["physical_kib"], master_pid, workers)
                    for phase, sample in phase_samples.items()
                }
                physical_summary["synchronous_peak"] = summarize_rss(
                    peak_physical_sample["physical_kib"], master_pid, workers
                )
            result = {
                "command": command,
                "master_pid": master_pid,
                "worker_pids": workers,
                "cold": cold,
                "hot": hot,
                "samples": samples,
                "peak_rss_kib": peak,
                "rss_summary": rss_summary,
                "physical_summary": physical_summary,
            }
            encoded = json.dumps(result, indent=2)
            if args.output:
                args.output.write_text(encoded + "\n")
            print(encoded)
        finally:
            server.terminate()
            try:
                server.wait(timeout=10)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait(timeout=5)
            log_file.close()
            stderr = log_path.read_text(encoding="utf-8", errors="replace")
            if stderr:
                print(stderr[-4000:], file=os.sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
