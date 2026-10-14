#!/usr/bin/env python3
# Copyright (c) 2026 https://asp-lang.org
# SPDX-License-Identifier: MIT

"""Measure PHP-FPM master and worker RSS with the same FastCGI load driver as OpenASP."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

from benchmark_memory import child_pids, memory_sample, run_load, run_native_load, summarize_rss


def wait_for_pool(server: subprocess.Popen[bytes], socket_path: Path, workers: int) -> list[int]:
    for _ in range(500):
        pids = child_pids(server.pid)
        if socket_path.exists() and len(pids) == workers:
            return pids
        if server.poll() is not None:
            raise RuntimeError("PHP-FPM exited before becoming ready")
        time.sleep(0.01)
    raise RuntimeError("PHP-FPM worker pool did not become ready")


def write_config(path: Path, socket_path: Path, session_path: Path, workers: int, opcache_memory: int) -> None:
    path.write_text(
        "[global]\n"
        "daemonize = no\n"
        "error_log = /dev/stderr\n"
        "\n"
        "[www]\n"
        f"listen = {socket_path}\n"
        "listen.mode = 0600\n"
        "pm = static\n"
        f"pm.max_children = {workers}\n"
        "pm.max_requests = 0\n"
        "catch_workers_output = yes\n"
        "decorate_workers_output = no\n"
        "clear_env = no\n"
        "security.limit_extensions = .php\n"
        "php_admin_flag[log_errors] = on\n"
        "php_admin_flag[display_errors] = off\n"
        "php_admin_flag[opcache.validate_timestamps] = off\n"
        f"php_admin_value[opcache.memory_consumption] = {opcache_memory}\n"
        f"php_admin_value[session.save_path] = {session_path}\n",
        encoding="ascii",
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--php-fpm", type=Path, default=Path(shutil.which("php-fpm") or "php-fpm"))
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--database", type=Path)
    parser.add_argument("--workers", type=int, default=1)
    parser.add_argument("--opcache-memory", type=int, default=16)
    parser.add_argument("--requests", type=int, default=1000)
    parser.add_argument("--concurrency", type=int, default=8)
    parser.add_argument("--sample-interval", type=float, default=0.25)
    parser.add_argument("--hold-seconds", type=float, default=0)
    parser.add_argument("--cookie", default="PHPBLOGSID=memory-benchmark")
    parser.add_argument("--cookie-mode", choices=("shared", "thread", "none"), default="shared")
    parser.add_argument("--paths", nargs="+", default=["default.php"])
    parser.add_argument("--load-generator", type=Path, default=Path("build/tests/bin/fastcgi-load"))
    parser.add_argument("--env", action="append", default=[], metavar="NAME=VALUE")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    php_fpm = args.php_fpm.resolve()
    root = args.root.resolve()
    load_generator = args.load_generator.resolve()
    database = args.database.resolve() if args.database else None
    if not php_fpm.is_file():
        raise RuntimeError(f"PHP-FPM binary does not exist: {php_fpm}")
    if not load_generator.is_file():
        raise RuntimeError(f"native FastCGI load generator does not exist: {load_generator}")
    if args.sample_interval <= 0:
        raise RuntimeError("sample interval must be positive")
    if database is not None and not database.is_file():
        raise RuntimeError(f"blog database does not exist: {database}")
    if args.workers < 1:
        raise RuntimeError("workers must be at least 1")

    with tempfile.TemporaryDirectory(prefix="php-fpm-memory-") as temporary:
        temporary_path = Path(temporary)
        socket_path = temporary_path / "php-fpm.sock"
        session_path = temporary_path / "sessions"
        session_path.mkdir()
        config_path = temporary_path / "php-fpm.conf"
        write_config(config_path, socket_path, session_path, args.workers, args.opcache_memory)

        command = [str(php_fpm), "-d", "opcache.enable=1", "-F", "-y", str(config_path)]
        environment = os.environ.copy()
        if database is not None:
            environment["PHP_ZBLOG_DB"] = str(database)
        for assignment in args.env:
            name, separator, value = assignment.partition("=")
            if not separator or not name:
                raise RuntimeError(f"invalid --env assignment: {assignment}")
            environment[name] = value
        log_path = temporary_path / "php-fpm.stderr"
        log_file = log_path.open("wb")
        server = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=log_file, env=environment)
        try:
            workers = wait_for_pool(server, socket_path, args.workers)
            pids = [server.pid, *workers]
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
                samples.append(memory_sample(pids, phase="held"))
            peak = {pid: max(int(sample["rss_kib"].get(pid, 0)) for sample in samples) for pid in pids}
            phase_samples = {str(sample["phase"]): sample for sample in samples if "phase" in sample}
            peak_sample = max(samples, key=lambda sample: sum(int(value) for value in sample["rss_kib"].values()))
            rss_summary = {
                phase: summarize_rss(sample["rss_kib"], server.pid, workers)
                for phase, sample in phase_samples.items()
            }
            rss_summary["synchronous_peak"] = summarize_rss(peak_sample["rss_kib"], server.pid, workers)
            physical_summary = None
            if all("physical_kib" in sample for sample in samples):
                peak_physical_sample = max(
                    samples, key=lambda sample: sum(int(value) for value in sample["physical_kib"].values())
                )
                physical_summary = {
                    phase: summarize_rss(sample["physical_kib"], server.pid, workers)
                    for phase, sample in phase_samples.items()
                }
                physical_summary["synchronous_peak"] = summarize_rss(
                    peak_physical_sample["physical_kib"], server.pid, workers
                )
            result = {
                "runtime": "php-fpm",
                "command": command,
                "master_pid": server.pid,
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
                args.output.parent.mkdir(parents=True, exist_ok=True)
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
