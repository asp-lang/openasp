#!/usr/bin/env python3
# Copyright (c) 2026 https://asp-lang.org
# SPDX-License-Identifier: MIT

"""Verify process workers and hybrid include bytecode invalidation."""

from __future__ import annotations

import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(next(parent for parent in Path(__file__).resolve().parents if (parent / "Makefile").is_file()) / "tests/integration/fastcgi"))
from test_fastcgi import transact


def wait_for_socket(server: subprocess.Popen[bytes], path: Path) -> None:
    for _ in range(200):
        if path.exists():
            return
        if server.poll() is not None:
            raise RuntimeError(server.stderr.read().decode())
        time.sleep(0.01)
    raise RuntimeError("FastCGI socket was not created")


def request(socket_path: Path, page: Path) -> bytes:
    return transact(
        socket_path,
        {
            "SCRIPT_FILENAME": str(page),
            "SCRIPT_NAME": f"/{page.name}",
            "REQUEST_METHOD": "GET",
            "QUERY_STRING": "",
            "SERVER_NAME": "localhost",
            "SERVER_PORT": "80",
        },
    )


def write_module(path: Path, version: str) -> None:
    path.write_text(
        "<% Function SharedValue()\n"
        f'SharedValue = "{version}"\n'
        "End Function\n"
        f'Response.Write "module-{version}"\n'
        "%>",
        encoding="utf-8",
    )


def process_children(parent: int) -> list[int]:
    output = subprocess.check_output(["ps", "-eo", "pid=,ppid="], text=True)
    children = []
    for line in output.splitlines():
        pid_text, parent_text = line.split()
        if int(parent_text) == parent:
            children.append(int(pid_text))
    return children


def main() -> int:
    binary = Path(sys.argv[1]).resolve()
    disassembler = Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(prefix="openasp-cache-") as temporary:
        root = Path(temporary)
        module = root / "shared.inc"
        entry_a = root / "a.asp"
        entry_b = root / "b.asp"
        fallback = root / "fallback.asp"
        close_block = root / "close.inc"
        transition = root / "transition.asp"
        transition_include = root / "transition.inc"
        scoped_entry = root / "scoped.asp"
        scoped_module = root / "scoped.inc"
        included_global_entry = root / "included_global.asp"
        included_global_module = root / "included_global.inc"
        windows_include_entry = root / "windows_include.asp"
        routine_include_entry = root / "routine_include.asp"
        routine_include_module = root / "routine_include.inc"
        hoisted_dim_entry = root / "hoisted_dim.asp"
        hoisted_dim_module = root / "hoisted_dim.inc"
        malformed_include_entry = root / "malformed_include.asp"
        invalid_expression_entry = root / "invalid_expression.asp"
        nested_directory = root / "nested"
        cache_directory = root / "opcache"
        nested_directory.mkdir()
        write_module(module, "v1")
        entry_a.write_text(
            '<%=SharedValue()%>|<!--#include file="shared.inc"-->',
            encoding="utf-8",
        )
        entry_b.write_text(
            'B:<%=SharedValue()%>|<!--#include file="shared.inc"-->',
            encoding="utf-8",
        )
        fallback.write_text(
            '<% If True Then %><!--#include file="close.inc"-->',
            encoding="utf-8",
        )
        close_block.write_text(
            '<% Response.Write "fallback"\nEnd If %>',
            encoding="utf-8",
        )
        transition.write_text(
            '<% If True Then %><!--#include file="transition.inc"-->'
            "<% End If %>",
            encoding="utf-8",
        )
        transition_include.write_text(
            '<% Response.Write "module" %>', encoding="utf-8"
        )
        scoped_module.write_text(
            '<%\nFunction UpdateShared()\n'
            'sharedValue = "updated"\n'
            'UpdateShared = True\n'
            'End Function\n%>',
            encoding="utf-8",
        )
        scoped_entry.write_text(
            '<% Dim sharedValue: sharedValue = "initial" %>'
            '<!--#include file="scoped.inc"-->'
            '<% If UpdateShared() Then Response.Write sharedValue %>',
            encoding="utf-8",
        )
        included_global_module.write_text(
            "<%\n"
            "Dim sharedItems()\n"
            "ReDim sharedItems(0)\n"
            'sharedItems(0) = "included-global"\n'
            "Dim sharedItem\n"
            "%>",
            encoding="utf-8",
        )
        included_global_entry.write_text(
            '<!--#include file="included_global.inc"-->'
            "<%\n"
            "Function ReadIncludedGlobal()\n"
            "For Each sharedItem In sharedItems\n"
            "ReadIncludedGlobal = sharedItem\n"
            "Next\n"
            "End Function\n"
            "Response.Write ReadIncludedGlobal()\n"
            "%>",
            encoding="utf-8",
        )
        (nested_directory / "value.inc").write_text(
            '<% Response.Write "windows-path" %>', encoding="utf-8"
        )
        windows_include_entry.write_text(
            '<!--#include file="nested\\value.inc"-->',
            encoding="utf-8",
        )
        routine_include_entry.write_text(
            "<%\n"
            "Function IncludedAssignment()\n"
            'Dim localValue: localValue = "initial"\n'
            "%>\n"
            '<!--#include file="routine_include.inc"-->\n'
            "<%\n"
            "IncludedAssignment = localValue\n"
            "End Function\n"
            "Response.Write IncludedAssignment()\n"
            "%>",
            encoding="utf-8",
        )
        routine_include_module.write_text(
            '<% localValue = "changed" %>', encoding="utf-8"
        )
        hoisted_dim_entry.write_text(
            "<%\n"
            "Option Explicit\n"
            "Response.Write IsEmpty(declaredByInclude)\n"
            "%>\n"
            '<!--#include file="hoisted_dim.inc"-->',
            encoding="utf-8",
        )
        hoisted_dim_module.write_text(
            "<% Dim declaredByInclude %>", encoding="utf-8"
        )
        malformed_include_entry.write_text(
            'before<!--#include typo="missing.inc"-->after',
            encoding="utf-8",
        )
        invalid_expression_entry.write_text(
            '<%= "first" : Response.Write "second" %>',
            encoding="utf-8",
        )

        plain_cache = root / "plain-opcache"
        plain_socket = root / "plain.sock"
        plain_server = subprocess.Popen(
            [
                str(binary),
                "--root",
                str(root),
                "--unix-socket",
                str(plain_socket),
                "--max-accepts",
                "1",
                "--workers",
                "1",
                "--opcache-dir",
                str(plain_cache),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        try:
            wait_for_socket(plain_server, plain_socket)
            assert b"\r\n\r\nv1|module-v1" in request(plain_socket, entry_a)
            assert plain_server.wait(timeout=5) == 0
        finally:
            if plain_server.poll() is None:
                plain_server.terminate()
                plain_server.wait(timeout=5)
        plain_images = list(plain_cache.glob("*.oac"))
        assert plain_images
        plain_disassembly = subprocess.check_output(
            [str(disassembler), str(plain_images[0])],
            text=True,
        )
        assert "Debug info: unavailable" in plain_disassembly
        assert "asp[" not in plain_disassembly
        assert "Dump of assembler code for function" in plain_disassembly

        socket_path = root / "cache.sock"
        server = subprocess.Popen(
            [
                str(binary),
                "--root",
                str(root),
                "--unix-socket",
                str(socket_path),
                "--max-accepts",
                "14",
                "--workers",
                "1",
                "--opcache-dir",
                str(cache_directory),
                "--gen-dbg-info=on",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        try:
            wait_for_socket(server, socket_path)
            assert b"\r\n\r\nv1|module-v1" in request(socket_path, entry_a)
            assert b"\r\n\r\nB:v1|module-v1" in request(socket_path, entry_b)
            assert b"\r\n\r\nmodule-v1" in request(socket_path, module)
            old_mtime = module.stat().st_mtime_ns
            write_module(module, "v2")
            os.utime(module, ns=(old_mtime + 1, old_mtime + 1))
            assert b"\r\n\r\nv2|module-v2" in request(socket_path, entry_a)
            assert b"\r\n\r\nupdated" in request(socket_path, scoped_entry)
            assert b"\r\n\r\nincluded-global" in request(
                socket_path, included_global_entry
            )
            assert b"\r\n\r\nwindows-path" in request(
                socket_path, windows_include_entry
            )
            routine_body = request(
                socket_path, routine_include_entry
            ).split(b"\r\n\r\n", 1)[1]
            assert routine_body.strip() == b"changed", routine_body
            hoisted_body = request(
                socket_path, hoisted_dim_entry
            ).split(b"\r\n\r\n", 1)[1]
            assert hoisted_body.strip() == b"True", hoisted_body
            assert request(
                socket_path, malformed_include_entry
            ).startswith(b"Status: 500")
            assert request(
                socket_path, invalid_expression_entry
            ).startswith(b"Status: 500")
            assert b"\r\n\r\nmodule" in request(socket_path, transition)
            transition_mtime = transition_include.stat().st_mtime_ns
            transition_include.write_text(
                '<% Else\nResponse.Write "other" %>', encoding="utf-8"
            )
            os.utime(
                transition_include,
                ns=(transition_mtime + 1, transition_mtime + 1),
            )
            transitioned = request(socket_path, transition)
            assert transitioned.endswith(b"\r\n\r\n"), transitioned
            fallback_output = request(socket_path, fallback)
            assert server.wait(timeout=5) == 0
            errors = server.stderr.read()
            assert b"\r\n\r\nfallback" in fallback_output, (
                fallback_output,
                errors,
            )
        finally:
            if server.poll() is None:
                server.terminate()
                server.wait(timeout=5)

        cache_files = list(cache_directory.glob("*.oac"))
        assert cache_files
        disassembly = subprocess.check_output(
            [str(disassembler), str(cache_files[0])],
            text=True,
        )
        for expected in (
            "OpenASP OAC disassembly",
            "Dependencies (",
            "Constants (",
            "Routines (",
            "Disassembly of section .oasp_bytecode:",
            "Dump of assembler code for function",
            "End of assembler dump.",
            "Debug info: present",
            "halt",
        ):
            assert expected in disassembly, expected
        assert "asp[" not in disassembly
        assert re.search(r"(?m)^\s+\d+\t.*\n\s+0x[0-9a-f]{16} ", disassembly)
        raw_instruction = re.compile(
            r"0x([0-9a-f]{16}) <[^>]+>:\s+"
            r"((?:[0-9a-f]{2} ){23}[0-9a-f]{2})\s+[a-z0-9_]+"
        )
        raw_match = raw_instruction.search(disassembly)
        assert raw_match, disassembly
        image_bytes = cache_files[0].read_bytes()
        instruction_offset = int(raw_match.group(1), 16)
        instruction_bytes = bytes.fromhex(raw_match.group(2))
        assert image_bytes[
            instruction_offset : instruction_offset + len(instruction_bytes)
        ] == instruction_bytes
        corrupt_image = root / "corrupt.oac"
        corrupt_image.write_bytes(image_bytes[:-1] + bytes([image_bytes[-1] ^ 0x01]))
        corrupt_result = subprocess.run(
            [str(disassembler), str(corrupt_image)],
            capture_output=True,
            check=False,
            text=True,
        )
        assert corrupt_result.returncode == 1, corrupt_result
        assert "not a valid OAC image" in corrupt_result.stderr
        truncated_image = root / "truncated.oac"
        truncated_image.write_bytes(image_bytes[: len(image_bytes) // 2])
        truncated_result = subprocess.run(
            [str(disassembler), str(truncated_image)],
            capture_output=True,
            check=False,
            text=True,
        )
        assert truncated_result.returncode == 1, truncated_result
        assert "not a valid OAC image" in truncated_result.stderr
        cold_socket = root / "cold.sock"
        module.chmod(0)
        server = subprocess.Popen(
            [
                str(binary),
                "--root",
                str(root),
                "--unix-socket",
                str(cold_socket),
                "--max-accepts",
                "1",
                "--workers",
                "1",
                "--opcache-dir",
                str(cache_directory),
                "--gen-dbg-info=on",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        try:
            wait_for_socket(server, cold_socket)
            assert b"\r\n\r\nv2|module-v2" in request(
                cold_socket, entry_a
            )
            assert server.wait(timeout=5) == 0
        finally:
            module.chmod(0o600)
            if server.poll() is None:
                server.terminate()
                server.wait(timeout=5)

        process_socket = root / "process.sock"
        server = subprocess.Popen(
            [
                str(binary),
                "--root",
                str(root),
                "--unix-socket",
                str(process_socket),
                "--workers",
                "2",
                "--max-requests",
                "2",
                "--memory-idle-ms",
                "20",
                "--memory-relief-requests",
                "1",
                "--opcache-dir",
                str(cache_directory),
                "--gen-dbg-info=on",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        try:
            wait_for_socket(server, process_socket)
            for _ in range(100):
                children = process_children(server.pid)
                if len(children) == 2:
                    break
                time.sleep(0.01)
            assert len(children) == 2, children
            original_children = set(children)
            for _ in range(8):
                assert b"\r\n\r\nB:v2|module-v2" in request(
                    process_socket, entry_b
                )
            for _ in range(100):
                children = process_children(server.pid)
                if len(children) == 2 and set(children) != original_children:
                    break
                time.sleep(0.01)
            assert len(children) == 2
            assert set(children) != original_children
            entry_mtime = entry_b.stat().st_mtime_ns
            entry_b.write_text(
                'B2:<%=SharedValue()%>|<!--#include file="shared.inc"-->',
                encoding="utf-8",
            )
            os.utime(entry_b, ns=(entry_mtime + 1, entry_mtime + 1))
            for _ in range(8):
                assert b"\r\n\r\nB2:v2|module-v2" in request(
                    process_socket, entry_b
                )
        finally:
            server.terminate()
            server.wait(timeout=5)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
