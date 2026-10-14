#!/usr/bin/env python3
"""Regression tests for the C signature parameter formatter."""

import importlib.util
import pathlib
import sys
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("format_c_parameters", ROOT / "scripts" / "format_c_parameters.py")
FORMATTER = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = FORMATTER
SPEC.loader.exec_module(FORMATTER)


class FormatCParametersTest(unittest.TestCase):
    def format(self, source: str) -> str:
        formatted, _lines = FORMATTER.format_text(source)
        return formatted

    def test_five_parameters_stay_on_one_line(self) -> None:
        source = "int five(int a, int b,\n         int c, int d, int e);\n"
        self.assertEqual(self.format(source), "int five(int a, int b, int c, int d, int e);\n")

    def test_sixth_parameter_starts_continuation_line(self) -> None:
        source = "int six(int a, int b, int c, int d, int e, int f);\n"
        expected = "int six(int a, int b, int c, int d, int e,\n        int f);\n"
        self.assertEqual(self.format(source), expected)

    def test_nested_function_pointer_does_not_count_its_parameters(self) -> None:
        source = "int nested(int a, void (*visit)(int x, int y), int c, int d, int e, int f);\n"
        expected = "int nested(int a, void (*visit)(int x, int y), int c, int d, int e,\n           int f);\n"
        self.assertEqual(self.format(source), expected)

    def test_function_call_is_not_formatted(self) -> None:
        source = "int wrapper(void) {\n    return six(a, b,\n               c, d, e, f);\n}\n"
        self.assertEqual(self.format(source), source)


if __name__ == "__main__":
    unittest.main()
