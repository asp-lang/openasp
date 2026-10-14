#!/usr/bin/env python3
"""Fuzz malformed ASP source through CLI and FastCGI without accepting crashes."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import random
import subprocess
import sys
import tempfile
import time
import urllib.request

ROOT = next(parent for parent in Path(__file__).resolve().parents if (parent / "Makefile").is_file())
sys.path.insert(0, str(ROOT / "tests/benchmarks/runners"))
sys.path.insert(0, str(ROOT / "tests/integration/fastcgi"))
__import__("sys").path.insert(0, str(next(parent for parent in Path(__file__).resolve().parents if (parent / "Makefile").is_file()) / "tests/benchmarks/runners"))
from benchmark_memory import darwin_physical_kib, rss_kib, wait_for_workers
from test_fastcgi import start_server, transact

SEEDS = (
    "<% If True Then\nResponse.Write \"x\" %>",
    "<% End If %>",
    "<% Function Broken(\nEnd Function %>",
    "<% Select Case 1\nCase\nEnd Select %>",
    "<% Dim value\nvalue = \"unterminated %>",
    "<% For i = 1 To\nNext %>",
    "<% Class Bad\nPublic Property Get Item(\nEnd Class %>",
    "<% On Error Resume Next\nIf Then\nEnd If %>",
    "<!--#include file=\"missing.inc\"-->\n<% Response.Write 1 %>",
    "<% Response.Write ((1 + ) %>",
)


def params(root: Path, name: str) -> dict[str, str]:
    return {
        "SCRIPT_FILENAME": str(root / name),
        "SCRIPT_NAME": "/" + name,
        "REQUEST_METHOD": "GET",
        "QUERY_STRING": "",
        "SERVER_NAME": "localhost",
        "SERVER_PORT": "80",
        "DOCUMENT_ROOT": str(root),
    }


def mutated_source(randomizer: random.Random, index: int) -> str:
    source = randomizer.choice(SEEDS)
    noise = "".join(randomizer.choice("()[]{}'\"=+-*/,:._ \t\r\n") for _ in range(randomizer.randrange(1, 80)))
    return f"<% ' fuzz-case {index}\n%>\n{source}\n<% ' mutation\n{noise}\n%>"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, default=Path("build/bin/openasp"))
    parser.add_argument("--fpm", type=Path, default=Path("build/bin/openasp-fpm"))
    parser.add_argument("--duration-seconds", type=float, default=300)
    parser.add_argument("--seed", type=int, default=20261011)
    parser.add_argument("--workers", type=int, default=4)
    parser.add_argument("--sample-seconds", type=float, default=15)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.duration_seconds <= 0 or args.workers <= 0:
        parser.error("duration and workers must be positive")

    binary = args.binary.resolve()
    fpm = args.fpm.resolve()
    randomizer = random.Random(args.seed)
    started = time.monotonic()
    errors: list[str] = []
    samples: list[dict[str, object]] = []
    cases = 0
    next_sample = started

    with tempfile.TemporaryDirectory(prefix="openasp-syntax-fuzz-") as raw:
        root = Path(raw).resolve()
        (root / "health.asp").write_text("<% Response.Write \"health-ok\" %>\n", encoding="utf-8")
        for index in range(512):
            (root / f"broken-{index}.asp").write_text(mutated_source(randomizer, index), encoding="utf-8")
        # macOS limits AF_UNIX paths to 104 bytes; TemporaryDirectory paths can
        # already consume most of that budget.
        socket_path = Path("/tmp") / f"oa-sf-{os.getpid()}.sock"
        socket_path.unlink(missing_ok=True)
        fpm_log = root / "fpm.log"
        diagnostics = (root / "fpm.stderr").open("wb")
        server = start_server(
            [
                str(fpm), "--root", str(root), "--unix-socket", str(socket_path), "--workers", str(args.workers),
                "--opcache-dir", str(root / "cache"), "--opcache-validate-timestamps=off",
                "--log-file", str(fpm_log),
            ],
            socket_path,
            stdout=subprocess.DEVNULL,
            stderr=diagnostics,
        )
        try:
            workers = wait_for_workers(server, socket_path, args.workers)
            while time.monotonic() - started < args.duration_seconds and not errors:
                name = f"broken-{cases % 512}.asp"
                path = root / name
                cli = subprocess.run([str(binary), str(path), "--root", str(root)], capture_output=True,
                                     timeout=5, check=False)
                if cli.returncode == 0:
                    errors.append(f"CLI accepted malformed case {cases}: {path.read_text()[:160]!r}")
                    break
                try:
                    response = transact(socket_path, params(root, name))
                except Exception as error:
                    errors.append(f"FPM transport failed after case {cases}: {error!r}")
                    break
                if b"Status: 500" not in response:
                    errors.append(f"FPM did not report syntax failure for case {cases}: {response[-300:]!r}")
                    break
                health = transact(socket_path, params(root, "health.asp"))
                if b"health-ok" not in health:
                    errors.append(f"FPM lost health after case {cases}: {health[-300:]!r}")
                    break
                if server.poll() is not None:
                    errors.append(f"FPM exited with status {server.returncode} after case {cases}")
                    break
                now = time.monotonic()
                if now >= next_sample:
                    pids = [server.pid, *workers]
                    samples.append({
                        "elapsed_seconds": round(now - started, 3),
                        "cases": cases,
                        "rss_kib": rss_kib(pids),
                        "physical_kib": darwin_physical_kib(pids) if __import__("os").uname().sysname == "Darwin" else {},
                    })
                    next_sample = now + args.sample_seconds
                cases += 1
        finally:
            if server.poll() is None:
                server.terminate()
                try:
                    server.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait(timeout=10)
            diagnostics.close()
            socket_path.unlink(missing_ok=True)

    elapsed = time.monotonic() - started
    report = {"seed": args.seed, "duration_seconds": elapsed, "cases": cases, "errors": errors, "samples": samples}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
