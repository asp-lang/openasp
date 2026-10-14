#!/usr/bin/env python3
# Copyright (c) 2026 https://asp-lang.org
# SPDX-License-Identifier: MIT

"""Monitor service workers created exclusively by OpenASP.Process.Fork()."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import tempfile
import time
import urllib.parse

__import__("sys").path.insert(0, str(next(parent for parent in Path(__file__).resolve().parents if (parent / "Makefile").is_file()) / "tests/benchmarks/runners"))
from benchmark_memory import darwin_physical_kib, rss_kib


GC_PATTERN = re.compile(
    r"OpenASP GC: heap=(?P<heap>\d+) external=(?P<external>\d+) "
    r"young=(?P<young>\d+) old=(?P<old>\d+) objects=(?P<objects>\d+) "
    r"minor=(?P<minor>\d+) major=(?P<major>\d+) promoted=(?P<promoted>\d+) "
    r"finalized=(?P<finalized>\d+) peak=(?P<peak>\d+) limit=(?P<limit>\d+) "
    r"allocation_failures=(?P<allocation_failures>\d+)"
)
RESULT_PATTERN = re.compile(
    r"^service-fork-gc-ok\|(?P<worker>\d+)\|(?P<processes>\d+)\|"
    r"(?P<iterations>\d+)\|(?P<checksum>\d+)\|(?P<mutations>\d+)\|"
    r"(?P<components>\d+)\|(?P<tail>-?\d+)\|(?P<children>\d+(?:,\d+)*)?$"
)


def process_table() -> dict[int, tuple[int, str]]:
    completed = subprocess.run(
        ["ps", "-axo", "pid=,ppid=,state="],
        check=False,
        capture_output=True,
        text=True,
    )
    table: dict[int, tuple[int, str]] = {}
    for line in completed.stdout.splitlines():
        fields = line.split()
        if len(fields) >= 3:
            table[int(fields[0])] = (int(fields[1]), fields[2])
    return table


def direct_children(parent: int, table: dict[int, tuple[int, str]]) -> list[int]:
    return sorted(pid for pid, (ppid, _) in table.items() if ppid == parent)


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
            counts[pid] = sum(
                line.startswith("f") for line in completed.stdout.splitlines()
            )
    return counts


def cpu_percentages(pids: list[int]) -> dict[int, float]:
    if not pids:
        return {}
    completed = subprocess.run(
        ["ps", "-o", "pid=,%cpu=", "-p", ",".join(map(str, pids))],
        check=False,
        capture_output=True,
        text=True,
    )
    result: dict[int, float] = {}
    for line in completed.stdout.splitlines():
        fields = line.split()
        if len(fields) == 2:
            result[int(fields[0])] = float(fields[1])
    return result


def metric_summary(
    samples: list[dict[str, object]], name: str, pid: int
) -> dict[str, int | None]:
    values = [
        int(sample[name][str(pid)])
        for sample in samples
        if str(pid) in sample[name]
    ]
    return {
        "initial": values[0] if values else None,
        "final": values[-1] if values else None,
        "minimum": min(values, default=None),
        "maximum": max(values, default=None),
        "growth": values[-1] - values[0] if values else None,
    }


def debug_events(path: Path, run_id: str) -> list[dict[str, object]]:
    events: list[dict[str, object]] = []
    if not path.exists():
        return events
    for line in path.read_text(errors="replace").splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if event.get("runId") == run_id:
            events.append(event)
    return events


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, default=Path("build/bin/openasp"))
    parser.add_argument(
        "--fixture",
        type=Path,
        default=Path("tests/fixtures/common/service_gc_megastress.asp"),
    )
    parser.add_argument("--processes", type=int, required=True)
    parser.add_argument("--iterations", type=int, default=14_000_000)
    parser.add_argument("--sample-seconds", type=float, default=5)
    parser.add_argument("--timeout-seconds", type=float, default=1200)
    parser.add_argument(
        "--debug-log",
        type=Path,
        default=Path(".dbg/trae-debug-log-service-fork-gc.ndjson"),
    )
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.processes < 1 or args.processes > 32 or args.iterations < 1:
        parser.error("processes must be 1..32 and iterations must be positive")

    binary = args.binary.resolve()
    fixture = args.fixture.resolve()
    output = args.output.resolve()
    run_id = os.environ.get("DEBUG_RUN_ID", f"fork-p{args.processes}")
    with tempfile.TemporaryDirectory(
        prefix=f"openasp-fork-gc-p{args.processes}-"
    ) as raw:
        temporary_path = Path(raw) / "component.tmp"
        query = urllib.parse.urlencode(
            {
                "role": "fork",
                "processes": args.processes,
                "iterations": args.iterations,
                "temp": temporary_path,
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
            "--gc-old-threshold=128K",
            "--process-enable=on",
        ]
        environment = {**os.environ, "LC_ALL": "C", "DEBUG_RUN_ID": run_id}
        started = time.monotonic()
        parent = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=environment,
            start_new_session=True,
        )
        deadline = started + args.timeout_seconds
        samples: list[dict[str, object]] = []
        observed_children: set[int] = set()
        timed_out = False

        while parent.poll() is None:
            now = time.monotonic()
            if now >= deadline:
                timed_out = True
                os.killpg(parent.pid, signal.SIGTERM)
                try:
                    parent.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    os.killpg(parent.pid, signal.SIGKILL)
                break
            table = process_table()
            children = direct_children(parent.pid, table)
            observed_children.update(children)
            live_pids = [parent.pid, *children]
            rss = rss_kib(live_pids)
            physical = (
                darwin_physical_kib(live_pids)
                if os.uname().sysname == "Darwin"
                else {}
            )
            fds = fd_counts(live_pids)
            cpu = cpu_percentages(live_pids)
            samples.append(
                {
                    "elapsed_seconds": round(now - started, 3),
                    "parent_pid": parent.pid,
                    "child_pids": children,
                    "states": {
                        str(pid): table[pid][1]
                        for pid in live_pids
                        if pid in table
                    },
                    "rss_kib": {str(pid): value for pid, value in rss.items()},
                    "physical_kib": {
                        str(pid): value for pid, value in physical.items()
                    },
                    "fd_count": {str(pid): value for pid, value in fds.items()},
                    "cpu_percent": {str(pid): value for pid, value in cpu.items()},
                }
            )
            time.sleep(args.sample_seconds)

        stdout_bytes, stderr_bytes = parent.communicate()
        elapsed = time.monotonic() - started
        stdout = stdout_bytes.decode("utf-8", errors="replace")
        stderr = stderr_bytes.decode("utf-8", errors="replace")
        matches = [
            RESULT_PATTERN.fullmatch(line.strip())
            for line in stdout.splitlines()
            if line.strip()
        ]
        results = [
            {
                key: (
                    [int(pid) for pid in value.split(",")]
                    if key == "children" and value
                    else [] if key == "children"
                    else int(value)
                )
                for key, value in match.groupdict().items()
            }
            for match in matches
            if match
        ]
        results.sort(key=lambda item: int(item["worker"]))
        final_gc = [
            {key: int(value) for key, value in match.groupdict().items()}
            for match in GC_PATTERN.finditer(stderr)
        ]
        events = debug_events(args.debug_log.resolve(), run_id)
        fork_events = [
            event for event in events if event.get("msg") == "[DEBUG] process-fork-returned"
        ]
        dispose_events = [
            event for event in events if event.get("msg") == "[DEBUG] service-dispose"
        ]
        reap_events = [
            event for event in events if event.get("msg") == "[DEBUG] fork-child-reaped"
        ]
        declared_children = (
            set(results[0]["children"])
            if results and int(results[0]["worker"]) == 0
            else set()
        )
        observed_workers = {parent.pid, *observed_children}
        zombie_samples = [
            float(sample["elapsed_seconds"])
            for sample in samples
            if any(str(state).startswith("Z") for state in sample["states"].values())
        ]

        errors: list[str] = []
        if timed_out:
            errors.append("parent timed out")
        if parent.returncode != 0:
            errors.append(f"parent exit code {parent.returncode}")
        if len(results) != args.processes:
            errors.append(f"expected {args.processes} result lines, got {len(results)}")
        if any(int(result["processes"]) != args.processes for result in results):
            errors.append("result process count mismatch")
        if [int(result["worker"]) for result in results] != list(
            range(args.processes)
        ):
            errors.append("worker indexes are incomplete or duplicated")
        result_signatures = {
            (
                int(result["iterations"]),
                int(result["checksum"]),
                int(result["mutations"]),
                int(result["components"]),
                int(result["tail"]),
            )
            for result in results
        }
        if results:
            expected_signature = (
                args.iterations,
                int(results[0]["checksum"]),
                args.iterations * 4,
                int(results[0]["components"]),
                int(results[0]["tail"]),
            )
            if result_signatures != {expected_signature}:
                errors.append("worker result signatures differ")
        if declared_children != observed_children:
            errors.append(
                f"declared children {sorted(declared_children)} differ from OS tree "
                f"{sorted(observed_children)}"
            )
        parent_forks = [
            event
            for event in fork_events
            if int(event["data"]["processPid"]) == parent.pid
            and event["data"]["role"] == "parent"
        ]
        child_forks = [
            event for event in fork_events if event["data"]["role"] == "child"
        ]
        if len(parent_forks) != args.processes - 1:
            errors.append(f"parent emitted {len(parent_forks)} fork events")
        if len(child_forks) != args.processes - 1:
            errors.append(f"children emitted {len(child_forks)} fork events")
        if len(dispose_events) != args.processes:
            errors.append(f"expected {args.processes} dispose events, got {len(dispose_events)}")
        if len(reap_events) != args.processes - 1:
            errors.append(f"expected {args.processes - 1} reap events, got {len(reap_events)}")
        if len(final_gc) != args.processes:
            errors.append(f"expected {args.processes} final GC lines, got {len(final_gc)}")
        if any(gc["allocation_failures"] != 0 for gc in final_gc):
            errors.append("GC allocation failures observed")
        if len({(gc["minor"], gc["major"]) for gc in final_gc}) > 1:
            errors.append("GC collection counts differ between processes")
        if zombie_samples:
            errors.append(f"zombies observed in periodic samples: {zombie_samples[:10]}")

        per_process = {
            str(pid): {
                "rss_kib": metric_summary(samples, "rss_kib", pid),
                "physical_kib": metric_summary(samples, "physical_kib", pid),
                "fd_count": metric_summary(samples, "fd_count", pid),
            }
            for pid in sorted(observed_workers)
        }
        report = {
            "model": "OpenASP.Process.Fork only",
            "processes": args.processes,
            "iterations_per_process": args.iterations,
            "total_iterations": args.processes * args.iterations,
            "elapsed_seconds": round(elapsed, 6),
            "aggregate_iterations_per_second": round(
                args.processes * args.iterations / elapsed, 3
            ),
            "command": command,
            "parent_pid": parent.pid,
            "declared_child_pids": sorted(declared_children),
            "observed_child_pids": sorted(observed_children),
            "results": results,
            "final_gc": final_gc,
            "dispose_gc_by_pid": {
                str(event["data"]["pid"]): event["data"] for event in dispose_events
            },
            "fork_events": fork_events,
            "reap_events": reap_events,
            "per_process": per_process,
            "samples": samples,
            "zombie_sample_count": len(zombie_samples),
            "stderr_tail": stderr[-2000:],
            "errors": errors,
        }
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(report, indent=2) + "\n")
        print(
            f"processes={args.processes} elapsed={elapsed:.3f}s "
            f"children={sorted(observed_children)} errors={len(errors)}"
        )
        for error in errors:
            print(f"error: {error}")
        return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
