#!/usr/bin/env python3
# Copyright (c) 2026 https://asp-lang.org
# SPDX-License-Identifier: MIT

"""Canonical paths shared by OpenASP test runners."""

from pathlib import Path


def find_repository_root(start: Path | None = None) -> Path:
    current = (start or Path(__file__)).resolve()
    for parent in (current, *current.parents):
        if (parent / "Makefile").is_file() and (parent / "src").is_dir():
            return parent
    raise RuntimeError(f"cannot locate the OpenASP repository from {current}")


REPOSITORY_ROOT = find_repository_root()
TESTS_ROOT = REPOSITORY_ROOT / "tests"
BUILD_ROOT = REPOSITORY_ROOT / "build"
TEST_BUILD_ROOT = BUILD_ROOT / "tests"
COMMON_FIXTURES = TESTS_ROOT / "fixtures" / "common"
REFERENCE_FIXTURES = TESTS_ROOT / "fixtures" / "reference"
CONFORMANCE_CASES = TESTS_ROOT / "conformance" / "cases"
E2E_APPS = TESTS_ROOT / "e2e" / "apps"
