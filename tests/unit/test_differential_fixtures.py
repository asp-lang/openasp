#!/usr/bin/env python3
# Copyright (c) 2026 https://asp-lang.org
# SPDX-License-Identifier: MIT

"""Verify differential fixture process isolation."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import tempfile


ROOT = next(parent for parent in Path(__file__).resolve().parents if (parent / "Makefile").is_file())


def load_runner():
    path = ROOT / "scripts/differential_fixtures.py"
    spec = importlib.util.spec_from_file_location("differential_fixtures", path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main() -> int:
    runner = load_runner()
    with tempfile.TemporaryDirectory(prefix="openasp-diff-unit-") as temporary:
        fixture_root = Path(temporary)
        fixture = fixture_root / "fixture.asp"
        fixture.write_text("<%= 42 %>\n", encoding="ascii")
        executable = fixture_root / "fake-reference"
        executable.write_text(
            "#!/bin/sh\n"
            "test ! -e :memory: || exit 73\n"
            "printf sqlite-test > :memory:\n",
            encoding="ascii",
        )
        executable.chmod(0o755)
        memory_file = fixture_root / ":memory:"
        memory_file.write_bytes(b"stale")

        result = runner.run_page(executable, fixture_root, fixture.name, 2.0)

        assert result["returncode"] == 0, result
        assert memory_file.read_bytes() == b"sqlite-test"
        assert not (ROOT / ":memory:").exists()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
