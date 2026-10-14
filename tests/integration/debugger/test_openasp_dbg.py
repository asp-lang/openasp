#!/usr/bin/env python3
# Copyright (c) 2026 https://asp-lang.org
# SPDX-License-Identifier: MIT

import pathlib
import subprocess
import sys
import tempfile


def run(command, *, input_text=None, expected=0):
    completed = subprocess.run(
        command,
        input=input_text,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=30,
        check=False,
    )
    if completed.returncode != expected:
        raise AssertionError(
            f"{command} returned {completed.returncode}, expected {expected}\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    return completed


def assert_contains(text, fragments):
    for fragment in fragments:
        if fragment not in text:
            raise AssertionError(f"missing {fragment!r} in:\n{text}")


def test_fork_debugging(debugger, root):
    fork_page = root / "debugger_fork.asp"
    command = [
        str(debugger),
        str(fork_page),
        "--root",
        str(root),
        "--process-enable=on",
    ]
    parent_run = run(
        command,
        input_text=(
            "show follow-fork-mode\n"
            "show detach-on-fork\n"
            "break 16\n"
            "continue\n"
            "print role\n"
            "info inferiors\n"
            "continue\n"
        ),
    )
    assert_contains(
        parent_run.stdout,
        [
            "The debugger follows the parent process after fork.",
            "Detaching after fork is on.",
            "[Following parent process ",
            "[Detaching after fork from child process ",
            '$ = "parent"',
            "(detached)",
        ],
    )

    child_run = run(
        command,
        input_text=(
            "set follow-fork-mode child\n"
            "break 16\n"
            "continue\n"
            "print role\n"
            "continue\n"
        ),
    )
    assert_contains(
        child_run.stdout,
        [
            "[Detaching after fork from parent process ",
            "[Following child process ",
            '$ = "child"',
        ],
    )

    both_run = run(
        command,
        input_text=(
            "set detach-on-fork off\n"
            "break 16\n"
            "continue\n"
            "print role\n"
            "info inferiors\n"
            "continue\n"
            "print role\n"
            "info inferiors\n"
            "continue\n"
        ),
    )
    assert_contains(
        both_run.stdout,
        [
            "[New inferior: parent process ",
            "[New inferior: child process ",
            '$ = "parent"',
            '$ = "child"',
        ],
    )
    if both_run.stdout.count("Breakpoint 1, ") != 2:
        raise AssertionError(
            "detach-on-fork off did not stop exactly once in each inferior:\n"
            f"{both_run.stdout}"
        )


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: test_openasp_dbg.py OPENASP OPENASP_DBG")
    openasp = pathlib.Path(sys.argv[1]).resolve()
    debugger = pathlib.Path(sys.argv[2]).resolve()
    root = next(parent for parent in pathlib.Path(__file__).resolve().parents if (parent / "Makefile").is_file()) / "tests/fixtures/common"
    page = root / "debugger_basic.asp"
    commands = (
        'print "literal-from-debugger"\n'
        "break AddValue\n"
        "continue\n"
        "backtrace\n"
        "info args\n"
        "print value\n"
        "set variable value = 41\n"
        "next\n"
        "next\n"
        "print local\n"
        "finish\n"
        "continue\n"
    )
    source_run = run(
        [str(debugger), str(page), "--root", str(root)],
        input_text=commands,
    )
    assert_contains(
        source_run.stdout,
        [
            '$ = "literal-from-debugger"',
            "Breakpoint 1 at AddValue",
            "*#0   AddValue",
            "value = 40",
            "$ = 42",
            "[Inferior output]\n42\n",
            "[Inferior exited normally]",
        ],
    )

    wrong_path_run = run(
        [str(debugger), str(page), "--root", str(root)],
        input_text="break /definitely/wrong/debugger_basic.asp:9\ncontinue\n",
    )
    if "Breakpoint 1, " in wrong_path_run.stdout:
        raise AssertionError(
            f"breakpoint with an explicit wrong path was matched:\n{wrong_path_run.stdout}"
        )

    error_run = run(
        [str(debugger), str(root / "debugger_error.asp"), "--root", str(root)],
        input_text="catch error\ncontinue\ninfo line\ncontinue\n",
        expected=1,
    )
    assert_contains(
        error_run.stdout,
        ["Catchpoint: ASP error 11: division by zero", "DIVIDE"],
    )

    if "minimal" not in debugger.parts:
        test_fork_debugging(debugger, root)

    with tempfile.TemporaryDirectory(prefix="openasp-dbg-") as directory:
        temporary = pathlib.Path(directory)
        image = temporary / "debugger_basic.oac"
        run(
            [
                str(openasp),
                str(page),
                "--root",
                str(root),
                "--emit-oac",
                str(image),
                "--gen-dbg-info=on",
            ]
        )
        image_run = run(
            [str(debugger), str(image), "--root", str(root)],
            input_text="break AddValue\ncontinue\ninfo args\ncontinue\n",
        )
        assert_contains(
            image_run.stdout,
            ["Breakpoint 1, ", 'value = 40', "[Inferior output]\n41\n"],
        )

        commands_file = temporary / "commands.dbg"
        commands_file.write_text("break 9\ncontinue\ninfo opcode\ncontinue\n")
        scripted = run(
            [
                str(debugger),
                str(page),
                "--root",
                str(root),
                "-x",
                str(commands_file),
            ]
        )
        assert_contains(scripted.stdout, ["Breakpoint 1, ", "WRITE_LOCAL"])

    print("openasp-dbg tests passed")


if __name__ == "__main__":
    main()
