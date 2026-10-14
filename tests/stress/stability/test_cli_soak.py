#!/usr/bin/env python3
"""Run one duration-driven OpenASP service workload and record resource samples."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import time
import urllib.parse

__import__("sys").path.insert(0, str(next(parent for parent in Path(__file__).resolve().parents if (parent / "Makefile").is_file()) / "tests/benchmarks/runners"))
from benchmark_memory import darwin_physical_kib, rss_kib


RESULT = re.compile(r"^stability-soak-ok\|(?P<duration>\d+)\|(?P<iterations>\d+)\|(?P<checksum>\d+)$")


def fd_count(pid: int) -> int | None:
    result = subprocess.run(["lsof", "-a", "-p", str(pid), "-Fn"], capture_output=True, text=True, check=False)
    return sum(line.startswith("f") for line in result.stdout.splitlines()) if result.returncode == 0 else None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, default=Path("build/bin/openasp"))
    parser.add_argument("--fixture", type=Path, default=Path("tests/fixtures/common/stability_service_soak.asp"))
    parser.add_argument("--duration-seconds", type=int, required=True)
    parser.add_argument("--sample-seconds", type=float, default=30)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.duration_seconds < 1 or args.duration_seconds > 21600 or args.sample_seconds <= 0:
        parser.error("duration must be 1..21600 seconds and sample interval must be positive")

    binary = args.binary.resolve()
    fixture = args.fixture.resolve()
    query = urllib.parse.urlencode({"duration": args.duration_seconds})
    command = [str(binary), str(fixture), "--root", str(fixture.parent), "--query", query,
               "--runtime-mode=service", "--gc-diagnostics", "--gc-old-threshold=128K"]
    started = time.monotonic()
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env={**os.environ, "LC_ALL": "C"})
    samples: list[dict[str, object]] = []
    deadline = started + args.duration_seconds + 120
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
            break
        sample = {
            "elapsed_seconds": round(now - started, 3),
            "rss_kib": rss_kib([process.pid]).get(process.pid),
            "physical_kib": darwin_physical_kib([process.pid]).get(process.pid) if os.uname().sysname == "Darwin" else None,
            "fd_count": fd_count(process.pid),
        }
        samples.append(sample)
        time.sleep(min(args.sample_seconds, max(0.05, deadline - time.monotonic())))

    stdout, stderr = process.communicate()
    elapsed = time.monotonic() - started
    match = RESULT.fullmatch(stdout.decode(errors="replace").strip())
    rss = [int(sample["rss_kib"]) for sample in samples if sample["rss_kib"] is not None]
    physical = [int(sample["physical_kib"]) for sample in samples if sample["physical_kib"] is not None]
    fds = [int(sample["fd_count"]) for sample in samples if sample["fd_count"] is not None]
    errors: list[str] = []
    if timed_out:
        errors.append("service workload exceeded duration grace period")
    if process.returncode != 0:
        errors.append(f"openasp exited with status {process.returncode}")
    if not match:
        errors.append(f"unexpected output: {stdout.decode(errors='replace')[-500:]!r}")
    if elapsed < args.duration_seconds:
        errors.append(f"workload ended before requested duration ({elapsed:.3f}s)")
    report = {
        "command": command,
        "duration_seconds": elapsed,
        "returncode": process.returncode,
        "result": match.groupdict() if match else None,
        "errors": errors,
        "rss_kib": {"initial": rss[0] if rss else None, "final": rss[-1] if rss else None,
                    "peak": max(rss, default=None), "growth": rss[-1] - rss[0] if rss else None},
        "physical_kib": {"initial": physical[0] if physical else None, "final": physical[-1] if physical else None,
                          "peak": max(physical, default=None), "growth": physical[-1] - physical[0] if physical else None},
        "fd_count": {"initial": fds[0] if fds else None, "final": fds[-1] if fds else None,
                     "peak": max(fds, default=None), "growth": fds[-1] - fds[0] if fds else None},
        "stderr_tail": stderr.decode(errors="replace")[-8000:],
        "samples": samples,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
