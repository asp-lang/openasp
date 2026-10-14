#!/usr/bin/env python3
# Copyright (c) 2026 https://asp-lang.org
# SPDX-License-Identifier: MIT

"""Verify direct OAC export from the openasp command."""

from __future__ import annotations

from pathlib import Path
import subprocess
import sys
import tempfile


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, capture_output=True, text=True, check=False)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    openasp = Path(sys.argv[1]).resolve()
    disassembler = Path(sys.argv[2]).resolve()
    fixture_root = (next(parent for parent in Path(__file__).resolve().parents if (parent / "Makefile").is_file()) / "tests/fixtures/common").resolve()
    page = fixture_root / "select_case_metadata.asp"

    for obsolete in (
        ["--log-file", "off"],
        ["--log-level", "notice"],
        ["--optimize=on"],
        ["--aot=on"],
    ):
        rejected = run([str(openasp), str(page), *obsolete])
        require(rejected.returncode == 2, f"{obsolete[0]} was accepted")
        require("Usage:" in rejected.stderr, f"{obsolete[0]} did not report usage")

    with tempfile.TemporaryDirectory(prefix="openasp-oac-") as temporary:
        output_root = Path(temporary)
        batch_image = output_root / "batch.oac"
        batch = run(
            [
                str(openasp),
                str(page),
                "--root",
                str(fixture_root),
                "--emit-oac",
                str(batch_image),
            ]
        )
        require(batch.returncode == 0, batch.stderr)
        require(batch.stdout == "third\n", f"unexpected batch output: {batch.stdout!r}")
        require(batch_image.is_file() and batch_image.stat().st_size > 0, "batch OAC was not created")
        batch_disassembly = run([str(disassembler), str(batch_image)])
        require(batch_disassembly.returncode == 0, batch_disassembly.stderr)
        require("Debug info: unavailable" in batch_disassembly.stdout, "default OAC unexpectedly contains debug source")
        require("Instructions: 66" in batch_disassembly.stdout, "batch OAC has unexpected bytecode")

        service_image = output_root / "service.oac"
        service = run(
            [
                str(openasp),
                str(page),
                "--root",
                str(fixture_root),
                "--runtime-mode=service",
                f"--emit-oac={service_image}",
                "--gen-dbg-info=on",
            ]
        )
        require(service.returncode == 0, service.stderr)
        require(service.stdout == "third\n", f"unexpected service output: {service.stdout!r}")
        service_disassembly = run([str(disassembler), str(service_image)])
        require(service_disassembly.returncode == 0, service_disassembly.stderr)
        require("Debug info: present" in service_disassembly.stdout, "debug OAC omitted source")
        require('Classify = "third"' in service_disassembly.stdout, "debug source mapping is unavailable")

        invalid_debug = run([str(openasp), str(page), "--gen-dbg-info=on"])
        require(invalid_debug.returncode == 2, "debug info without OAC did not fail usage validation")
        require("requires --emit-oac FILE" in invalid_debug.stderr, "missing actionable debug option error")

        invalid_suffix = run([str(openasp), str(page), "--emit-oac", str(output_root / "page.bin")])
        require(invalid_suffix.returncode == 2, "non-OAC destination was accepted")
        require("must end in .oac" in invalid_suffix.stderr, "missing OAC suffix error")

        missing_parent = output_root / "missing" / "page.oac"
        unwritable = run([str(openasp), str(page), "--root", str(fixture_root), "--emit-oac", str(missing_parent)])
        require(unwritable.returncode == 1, "unwritable OAC destination did not fail")
        require("check the destination directory and permissions" in unwritable.stderr, "missing write recovery advice")
        require(not list(output_root.glob("*.tmp.*")), "failed export left a temporary file")

    print("openasp OAC export tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
