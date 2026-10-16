#!/usr/bin/env python3
# Copyright (c) 2026 https://asp-lang.org
# SPDX-License-Identifier: MIT

"""Run the self-contained ASP language and component conformance fixtures."""

from __future__ import annotations

from pathlib import Path
import os
import subprocess
import sys
import tempfile
import urllib.parse

sys.path.insert(0, str(next(parent for parent in Path(__file__).resolve().parents if (parent / "Makefile").is_file()) / "tests/integration/fastcgi"))
from test_fastcgi import start_server, transact


ROOT = next(parent for parent in Path(__file__).resolve().parents if (parent / "Makefile").is_file())
FIXTURES = ROOT / "tests" / "conformance" / "cases"


def run_cli(binary: Path, name: str, *options: str) -> str:
    result = subprocess.run(
        [str(binary), str(FIXTURES / name), "--root", str(FIXTURES), *options],
        capture_output=True,
        text=True,
        timeout=10,
        check=False,
    )
    assert result.returncode == 0, f"{name}: {result.stderr}"
    return result.stdout


def assert_cli(binary: Path, name: str, expected: str, *options: str) -> None:
    actual = run_cli(binary, name, *options)
    assert actual == expected, f"{name}: expected {expected!r}, got {actual!r}"
    print(f"PASS cli/{name}")


def assert_cli_fails(binary: Path, name: str, expected_error: str) -> None:
    result = subprocess.run(
        [str(binary), str(FIXTURES / name), "--root", str(FIXTURES)],
        capture_output=True,
        text=True,
        timeout=10,
        check=False,
    )
    assert result.returncode != 0, f"{name}: unexpectedly succeeded"
    assert expected_error in result.stderr, (
        f"{name}: expected {expected_error!r} in {result.stderr!r}"
    )
    print(f"PASS cli-error/{name}")


def assert_cli_cookie_subkeys(binary: Path) -> None:
    environment = os.environ.copy()
    environment["HTTP_COOKIE"] = "ACTCMS=UserName=member&PassWord=secret&GroupID=3"
    result = subprocess.run(
        [str(binary), str(FIXTURES / "request_cookie_subkeys.asp"), "--root", str(FIXTURES)],
        capture_output=True,
        text=True,
        timeout=10,
        check=False,
        env=environment,
    )
    assert result.returncode == 0, result.stderr
    assert result.stdout == "member:secret:\n", result.stdout
    print("PASS cli/request_cookie_subkeys.asp")


def assert_ado_recordset_lifetime(binary: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="openasp-ado-lifetime-") as temporary:
        database = Path(temporary) / "recordset.db"
        query = urllib.parse.urlencode({"db": str(database)})
        assert_cli(
            binary,
            "ado_recordset_lifetime.asp",
            "alpha\n",
            "--query",
            query,
        )


def assert_fastcgi(binary: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="openasp-conformance-") as temporary:
        temporary_path = Path(temporary)
        socket_path = temporary_path / "openasp.sock"
        server = start_server(
            [
                str(binary),
                "--root",
                str(FIXTURES),
                "--unix-socket",
                str(socket_path),
                "--max-accepts",
                "1",
                "--workers",
                "1",
                "--opcache-dir",
                str(temporary_path / "opcache"),
            ],
            socket_path,
        )
        try:
            response = transact(
                socket_path,
                {
                    "SCRIPT_FILENAME": str(FIXTURES / "request_response.asp"),
                    "SCRIPT_NAME": "/request_response.asp",
                    "REQUEST_METHOD": "POST",
                    "QUERY_STRING": urllib.parse.urlencode({"name": "FastCGI"}),
                    "SERVER_NAME": "localhost",
                    "SERVER_PORT": "80",
                    "CONTENT_TYPE": "application/x-www-form-urlencoded",
                },
                b"value=body",
            )
            headers, body = response.split(b"\r\n\r\n", 1)
            assert b"Status: 201 Created" in headers
            assert b"Content-Type: text/plain; charset=utf-8" in headers
            assert b"X-Conformance: ok" in headers
            assert b"Set-Cookie: mode=tested; Path=/" in headers
            assert body == b"FastCGI|body|POST\n", response
            assert server.wait(timeout=5) == 0
        finally:
            if server.poll() is None:
                server.terminate()
                server.wait(timeout=5)
    print("PASS fpm/request_response")


def main() -> int:
    openasp = Path(sys.argv[1]).resolve()
    fpm = Path(sys.argv[2]).resolve()
    assert_cli(openasp, "expressions.asp", "8|3|2|-1|-1|-1|-1|a1:11:True\n")
    assert_cli(openasp, "control_flow.asp", "flow|3|matched|True\n")
    assert_cli(openasp, "routines_classes.asp", "5|7|5|7\n")
    assert_cli(openasp, "with_exit.asp", "7|12|True\n")
    assert_cli(openasp, "syntax_statements.asp", "3|2|9|elseif-caseABC\n")
    assert_cli(
        openasp,
        "syntax_builtins.asp",
        "True|2|3|4.5| abc |ABC|x-b-x|ab|ef|bcd|1|a:b:c|0|2|2|5|0|65|66|3|FF|2|-1|-3|-2|2024-2-29|2|True|True|True|Variant()|8204\n",
    )
    assert_cli(openasp, "dynamic_error_paths.asp", "42|43|44|11|429\n")
    assert_cli(
        openasp,
        "resume_next_conditions.asp",
        "if:0:91|loop:0:91|branches:0:91|post:1:91|empty:kept:424\n",
    )
    assert_cli(openasp, "resume_next_call_scope.asp", "0|0|0|0|continued\n")
    assert_cli(openasp, "components.asp", "one|a1|payload|<root id=\"7\"/>\n")
    assert_cli(
        openasp,
        "component_local_members.asp",
        "True:2:beta:one|0|True:zx|1:A1:1:2|abcd:cd:True:1|True:{\"a\":1}:{\"a\":1}|YWJj:abc:Pz8|True:line2\n:False|4:0:child:text:<child id=\"7\" name=\"node\">text</child>:1\n",
    )
    assert_cli(openasp, "component_error_paths.asp", "3704|5|5|438\n")
    assert_cli(openasp, "fso_open_text_modes.asp", "first-second\n\n")
    assert_cli(openasp, "ado_stream_flush.asp", "flushed\n")
    assert_cli(openasp, "ado_stream_utf8_bom_copy.asp", "<meta>\n")
    assert_cli(openasp, "ado_fields_enumeration.asp", "2;title:first|rank:7|\n")
    assert_cli(openasp, "ado_recordset_addnew.asp", "first:7:1\n")
    assert_cli(openasp, "xml_attribute_map.asp", "<row title=\"first\"/>\n")
    assert_ado_recordset_lifetime(openasp)
    assert_cli(
        openasp,
        "component_factories.asp",
        "|".join(["True"] * 21) + "\n",
    )
    assert_cli(
        openasp,
        "privileged_components.asp",
        "True|70\n",
        "--process-enable=on",
    )
    assert_cli(openasp, "preprocessor.asp", "before\nafter-included\n")
    assert_cli(
        openasp,
        "request_response.asp",
        "CLI|body|POST\n",
        "--query",
        "name=CLI",
        "--body",
        "value=body",
        "--method",
        "POST",
    )
    assert_cli_cookie_subkeys(openasp)
    assert_cli_fails(openasp, "invalid_syntax.asp", "syntax error: expected End If")
    assert_cli_fails(
        openasp,
        "preprocessor_missing_include.asp",
        "preprocess error: include file does not exist",
    )
    assert_fastcgi(fpm)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
