#!/usr/bin/env python3
# Copyright (c) 2026 https://asp-lang.org
# SPDX-License-Identifier: MIT

"""Benchmark equivalent ASP and PHP blog workloads under matched FastCGI settings."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import sys
import tempfile


ROOT = next(parent for parent in Path(__file__).resolve().parents if (parent / "Makefile").is_file())
ASP_SOURCE = ROOT / "tests/benchmarks/workloads/openasp/new-asp-zblog"
PHP_SOURCE = ROOT / "tests/benchmarks/workloads/php/php-zblog"
CASES = (
    ("default", ""),
    ("search", "q=runtime"),
    ("tags", "tag=runtime"),
    ("feed", ""),
    ("view", "id=1"),
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--openasp-fpm", type=Path, default=ROOT / "build" / "bin" / "openasp-fpm")
    parser.add_argument("--openasp-cli", type=Path, default=ROOT / "build" / "bin" / "openasp")
    parser.add_argument("--php-fpm", type=Path, default=Path(shutil.which("php-fpm") or "php-fpm"))
    parser.add_argument("--php", type=Path, default=Path(shutil.which("php") or "php"))
    parser.add_argument("--workers", type=int, nargs="+", default=[1, 5, 10])
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--requests", type=int, default=1000)
    parser.add_argument("--concurrency", type=int, default=16)
    parser.add_argument("--sample-interval", type=float, default=0.25)
    parser.add_argument("--pages", nargs="+", choices=[page for page, _ in CASES])
    parser.add_argument("--output-dir", type=Path, default=ROOT / "build" / "benchmarks" / "zblog")
    return parser.parse_args()


def canonical_body(output: bytes) -> bytes:
    body = output.split(b"\r\n\r\n", 1)[-1]
    body = body.replace(b".asp", b".page").replace(b".php", b".page")
    body = body.replace(b"OpenASP-C", b"runtime").replace(b"PHP-FPM", b"runtime")
    body = re.sub(rb">\s+<", b"><", body.strip())
    return body


def validate_outputs(openasp: Path, php: Path, asp_root: Path, php_root: Path,
                     database: Path, session_path: Path,
                     cases: tuple[tuple[str, str], ...] = CASES) -> list[dict[str, object]]:
    results: list[dict[str, object]] = []
    php_runner = (
        "parse_str($argv[1], $_GET); "
        "$_SERVER['REQUEST_METHOD']='GET'; "
        "$_SERVER['SERVER_NAME']='localhost'; "
        "include $argv[2];"
    )
    environment = {**os.environ, "PHP_ZBLOG_DB": str(database)}
    for page, query in cases:
        asp_command = [
            str(openasp), str(asp_root / f"{page}.asp"), "--root", str(asp_root),
            "--query", query,
        ]
        php_command = [
            str(php), "-d", f"session.save_path={session_path}", "-r", php_runner,
            query, str(php_root / f"{page}.php"),
        ]
        asp = subprocess.run(asp_command, check=True, capture_output=True)
        php_run = subprocess.run(php_command, check=True, capture_output=True, env=environment)
        asp_body = canonical_body(asp.stdout)
        php_body = canonical_body(php_run.stdout)
        if asp_body != php_body:
            mismatch = next(
                (index for index, pair in enumerate(zip(asp_body, php_body)) if pair[0] != pair[1]),
                min(len(asp_body), len(php_body)),
            )
            raise RuntimeError(
                f"{page} output mismatch at byte {mismatch}: "
                f"asp={asp_body[mismatch:mismatch + 120]!r} php={php_body[mismatch:mismatch + 120]!r}"
            )
        results.append({
            "page": page,
            "query": query,
            "bytes": len(asp_body),
            "sha256": hashlib.sha256(asp_body).hexdigest(),
        })
    return results


def run_benchmark(command: list[str], output: Path) -> dict[str, object]:
    output.parent.mkdir(parents=True, exist_ok=True)
    completed = subprocess.run(command + ["--output", str(output)], check=False, capture_output=True, text=True)
    if completed.returncode:
        raise RuntimeError(
            f"benchmark failed ({completed.returncode}): {' '.join(command)}\n"
            f"stdout:\n{completed.stdout[-4000:]}\nstderr:\n{completed.stderr[-4000:]}"
        )
    if completed.stderr:
        sys.stderr.write(completed.stderr)
    return json.loads(output.read_text(encoding="utf-8"))


def summarize(runs: list[dict[str, object]]) -> dict[str, float]:
    rates = [float(run["hot"]["requests_per_second"]) for run in runs]
    rss = [int(run["rss_summary"]["hot"]["total_kib"]) for run in runs]
    physical = [
        int(run["physical_summary"]["hot"]["total_kib"])
        for run in runs
        if run.get("physical_summary")
    ]
    result = {
        "requests_per_second_median": statistics.median(rates),
        "requests_per_second_min": min(rates),
        "requests_per_second_max": max(rates),
        "hot_rss_kib_median": statistics.median(rss),
    }
    if physical:
        result["hot_physical_kib_median"] = statistics.median(physical)
    return result


def main() -> int:
    args = parse_args()
    selected_cases = tuple(case for case in CASES if not args.pages or case[0] in args.pages)
    for path in (args.openasp_fpm, args.openasp_cli, args.php_fpm, args.php):
        if not path.is_file():
            raise RuntimeError(f"required executable does not exist: {path}")
    if args.rounds < 1:
        raise RuntimeError("--rounds must be positive")
    if args.requests < 1 or args.concurrency < 1:
        raise RuntimeError("requests and concurrency must be positive")

    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    database = output_dir / "blog.sqlite"
    subprocess.run([str(args.php), str(PHP_SOURCE / "setup.php"), str(database)],
                   check=True, stdout=subprocess.DEVNULL)

    with tempfile.TemporaryDirectory(prefix="openasp-zblog-") as temporary:
        temporary_path = Path(temporary)
        asp_root = temporary_path / "asp"
        php_root = temporary_path / "php"
        session_path = temporary_path / "sessions"
        shutil.copytree(ASP_SOURCE, asp_root)
        shutil.copytree(PHP_SOURCE, php_root)
        session_path.mkdir()
        (asp_root / "blog.sqlite").symlink_to(database)
        validation = validate_outputs(args.openasp_cli.resolve(), args.php.resolve(), asp_root, php_root,
                                      database, session_path, selected_cases)

        all_runs: dict[str, dict[str, list[dict[str, object]]]] = {"openasp-c": {}, "php-fpm": {}}
        asp_paths = [f"{page}.asp" + (f"?{query}" if query else "") for page, query in selected_cases]
        php_paths = [f"{page}.php" + (f"?{query}" if query else "") for page, query in selected_cases]
        for workers in args.workers:
            worker_key = str(workers)
            all_runs["openasp-c"][worker_key] = []
            all_runs["php-fpm"][worker_key] = []
            for round_index in range(args.rounds):
                common = [
                    "--workers", worker_key,
                    "--requests", str(args.requests),
                    "--concurrency", str(args.concurrency),
                    "--sample-interval", str(args.sample_interval),
                    "--cookie-mode", "thread",
                ]
                asp_output = output_dir / f"openasp-w{workers}-r{round_index + 1}.json"
                php_output = output_dir / f"php-w{workers}-r{round_index + 1}.json"
                asp_command = [
                    sys.executable, str(ROOT / "tests" / "benchmark_memory.py"),
                    "--binary", str(args.openasp_fpm.resolve()),
                    "--root", str(asp_root),
                    *common, "--paths", *asp_paths,
                ]
                php_command = [
                    sys.executable, str(ROOT / "tests" / "benchmark_php_fpm.py"),
                    "--php-fpm", str(args.php_fpm.resolve()),
                    "--root", str(php_root),
                    "--database", str(database),
                    *common, "--paths", *php_paths,
                ]
                all_runs["openasp-c"][worker_key].append(run_benchmark(asp_command, asp_output))
                all_runs["php-fpm"][worker_key].append(run_benchmark(php_command, php_output))

    summaries: dict[str, dict[str, dict[str, float]]] = {"openasp-c": {}, "php-fpm": {}}
    comparison: dict[str, dict[str, float]] = {}
    for workers in args.workers:
        key = str(workers)
        summaries["openasp-c"][key] = summarize(all_runs["openasp-c"][key])
        summaries["php-fpm"][key] = summarize(all_runs["php-fpm"][key])
        asp_rate = summaries["openasp-c"][key]["requests_per_second_median"]
        php_rate = summaries["php-fpm"][key]["requests_per_second_median"]
        comparison[key] = {
            "openasp_to_php_ratio": asp_rate / php_rate,
            "php_over_openasp": php_rate / asp_rate,
        }
    report = {
        "conditions": {
            "workers": args.workers,
            "rounds": args.rounds,
            "requests_per_round": args.requests,
            "concurrency": args.concurrency,
            "cookie_mode": "thread",
            "pages": [page for page, _ in selected_cases],
        },
        "validation": validation,
        "summary": summaries,
        "comparison": comparison,
        "runs": all_runs,
    }
    report_path = output_dir / "comparison.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    database.unlink()
    print(json.dumps({"summary": summaries, "comparison": comparison, "report": str(report_path)}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
