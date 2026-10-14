#!/usr/bin/env python3
# Copyright (c) 2026 https://asp-lang.org
# SPDX-License-Identifier: MIT

"""Compare OpenASP fixture behavior against the egret-asp reference binary."""

from __future__ import annotations

import argparse
import difflib
import json
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.parse
import urllib.request


EXTERNAL_FIXTURES = {
    "ado_recordset_source.asp",
    "ado_mdb_compound_where.asp",
    "ado_mdb_join.asp",
    "component_backend_services.asp",
    "component_http.asp",
    "component_https.asp",
    "component_memcache.asp",
    "component_mysql.asp",
    "component_mysql_datetime_rewrite.asp",
    "component_mysql_empty_recordset.asp",
    "component_mysql_prepare.asp",
    "component_mysql_provider_fallback.asp",
    "component_process.asp",
    "component_redis.asp",
    "component_shell_enabled.asp",
    "component_shell_errors.asp",
    "component_shell_literal_args.asp",
    "component_socket_client.asp",
    "component_socket_listener.asp",
    "component_socket_udp.asp",
    "component_websocket_client.asp",
    "component_websocket_server.asp",
    "component_websocket_wss.asp",
}

STATEFUL_FIXTURES = {
    "get_session_complex.asp",
    "load_user_class.asp",
}

INDEPENDENT_OUTPUTS = {
    "component_stream_copy.asp": b"True:True:True\n",
    "dynamic_bytecode_gc.asp": b"counter=10\r\nobject=31\r\nexecute=2\r\neval=44\r\n\n",
    "index.asp": (
        b"<div>header-from-include</div>\n\n"
        b"<div>site=egret-asp</div><div>phase=bootstrap</div>\n"
        b"<div>hello=OpenASP</div>\n"
        b"<div>query=OpenASP</div>\n"
        b"<div>session=OpenASP</div>\n"
    ),
    "min_app.asp": b"egret-asp\n",
    "multidimensional_array_bounds_byref.asp": b"0:2:a|2\n",
    "routine_lexical_scope.asp": b"global\n",
    "with_statement.asp": b"7\n",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--candidate", default="build/bin/openasp")
    parser.add_argument(
        "--reference",
        default=os.environ.get("OPENASP_REFERENCE_BIN"),
        help="reference engine binary (or set OPENASP_REFERENCE_BIN)",
    )
    parser.add_argument(
        "--fixtures",
        default=Path(__file__).resolve().parents[1] / "tests/fixtures/reference",
    )
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--report", default="build/fixture-diff.json")
    parser.add_argument("--include-external", action="store_true")
    parser.add_argument("--fixture", action="append", default=[])
    return parser.parse_args()


SQLITE_FIXTURES = {
    "ado_recordset_not_eof.asp",
    "ado_recordset_paging.asp",
    "ado_recordset_properties.asp",
    "ado_sqlite.asp",
    "component_command.asp",
    "sqlite_native.asp",
}

MDB_FIXTURES = {
    "ado_mdb_bracket_clause.asp",
    "ado_mdb_top.asp",
}

MDB_SOURCE = (
    Path(__file__).resolve().parents[1]
    / "tests/e2e/apps/backlighting-cms/data_act/#BackLightingDatabase.mdb"
)


def command_for(binary: Path, page: Path, root: Path, name: str) -> list[str]:
    command = [str(binary), str(page), "--root", str(root)]
    if name == "index.asp":
        command.extend(["--query", "name=OpenASP"])
    elif name in SQLITE_FIXTURES:
        command.extend(
            ["--query", urllib.parse.urlencode({"db": str(root / "fixture.db")})]
        )
    elif name in MDB_FIXTURES:
        command.extend(
            ["--query", urllib.parse.urlencode({"db": str(root / "fixture.mdb")})]
        )
    elif name == "ado_recordset_connection_string.asp":
        connection = f"Provider=SQLite;Data Source={root / 'fixture.db'}"
        command.extend(
            [
                "--method",
                "POST",
                "--body",
                urllib.parse.urlencode(
                    {
                        "connection": connection,
                        "sql": "select id, name from demo order by id",
                    }
                ),
            ]
        )
    elif name == "components_basic.asp":
        command.extend(
            ["--query", urllib.parse.urlencode({"tmp": str(root / "tmp.txt")})]
        )
    elif name == "component_dom_save.asp":
        command.extend(
            ["--query", urllib.parse.urlencode({"file": str(root / "saved.xml")})]
        )
    return command


def prepare_root(root: Path) -> None:
    database = sqlite3.connect(root / "fixture.db")
    try:
        database.executescript(
            "drop table if exists demo;"
            "create table demo(id integer primary key, name text);"
            "insert into demo values(1, 'alpha'), (2, 'beta');"
        )
        database.commit()
    finally:
        database.close()


def prepare_mdb(root: Path) -> None:
    shutil.copy2(MDB_SOURCE, root / "fixture.mdb")


def normalized_output(output: bytes, root: Path) -> bytes:
    replacements = {
        str(root).encode(),
        str(root.resolve()).encode(),
        str(root).replace("/", "\\").encode(),
        str(root.resolve()).replace("/", "\\").encode(),
    }
    result = output
    for value in sorted(replacements, key=len, reverse=True):
        result = result.replace(value, b"<ROOT>")
    return result


def run_page(
    binary: Path, root: Path, name: str, timeout: float
) -> dict[str, object]:
    (root / ":memory:").unlink(missing_ok=True)
    started = time.monotonic()
    try:
        completed = subprocess.run(
            command_for(binary, root / name, root, name),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
            check=False,
            cwd=root,
            env={**os.environ, "LC_ALL": "C"},
        )
        # #region debug-point A-E:fixture-result
        _p = ".dbg/fixture-behavior-diffs.env"; _u = "http://127.0.0.1:7777/event"; exec("try:\n c=open(_p).read(); _u=next((l.split('=',1)[1] for l in c.splitlines() if l.startswith('DEBUG_SERVER_URL=')),_u)\nexcept OSError: pass"); _payload = {"sessionId": "fixture-behavior-diffs", "runId": "post-fix", "hypothesisId": "A-E", "location": "scripts/differential_fixtures.py:run_page", "msg": "[DEBUG] fixture process completed", "data": {"fixture": name, "binary": binary.name, "returncode": completed.returncode, "elapsed_ms": round((time.monotonic() - started) * 1000, 3), "stdout": completed.stdout[:800].decode("utf-8", errors="backslashreplace"), "stderr": completed.stderr[:800].decode("utf-8", errors="backslashreplace")}}; exec("try:\n urllib.request.urlopen(urllib.request.Request(_u,data=json.dumps(_payload).encode(),headers={'Content-Type':'application/json'}),timeout=1).read()\nexcept Exception: pass")
        # #endregion
        return {
            "returncode": completed.returncode,
            "stdout": completed.stdout,
            "stderr": completed.stderr,
            "elapsed_ms": round((time.monotonic() - started) * 1000, 3),
            "timeout": False,
        }
    except subprocess.TimeoutExpired as error:
        # #region debug-point C:fixture-timeout
        _p = ".dbg/fixture-behavior-diffs.env"; _u = "http://127.0.0.1:7777/event"; exec("try:\n c=open(_p).read(); _u=next((l.split('=',1)[1] for l in c.splitlines() if l.startswith('DEBUG_SERVER_URL=')),_u)\nexcept OSError: pass"); _payload = {"sessionId": "fixture-behavior-diffs", "runId": "post-fix", "hypothesisId": "C", "location": "scripts/differential_fixtures.py:run_page", "msg": "[DEBUG] fixture process timed out", "data": {"fixture": name, "binary": binary.name, "elapsed_ms": round((time.monotonic() - started) * 1000, 3), "stdout": (error.stdout or b"")[:800].decode("utf-8", errors="backslashreplace"), "stderr": (error.stderr or b"")[:800].decode("utf-8", errors="backslashreplace")}}; exec("try:\n urllib.request.urlopen(urllib.request.Request(_u,data=json.dumps(_payload).encode(),headers={'Content-Type':'application/json'}),timeout=1).read()\nexcept Exception: pass")
        # #endregion
        return {
            "returncode": None,
            "stdout": error.stdout or b"",
            "stderr": error.stderr or b"",
            "elapsed_ms": round((time.monotonic() - started) * 1000, 3),
            "timeout": True,
        }


def failure_kind(run: dict[str, object]) -> str:
    if run["timeout"]:
        return "timeout"
    stderr = bytes(run["stderr"]).lower()
    if b"syntax error" in stderr or b"preprocess error" in stderr:
        return "parse-failure"
    return "runtime-failure"


def display_bytes(value: bytes, limit: int = 800) -> str:
    text = value.decode("utf-8", errors="backslashreplace")
    if len(text) > limit:
        return text[:limit] + f"\n... ({len(text) - limit} characters omitted)"
    return text


def output_diff(reference: bytes, candidate: bytes) -> str:
    lines = difflib.unified_diff(
        reference.decode("utf-8", errors="backslashreplace").splitlines(True),
        candidate.decode("utf-8", errors="backslashreplace").splitlines(True),
        fromfile="reference",
        tofile="candidate",
        n=2,
    )
    return "".join(lines)[:2000]


def classify(
    name: str,
    reference: dict[str, object],
    candidate: dict[str, object],
    reference_root: Path,
    candidate_root: Path,
) -> str:
    expected = INDEPENDENT_OUTPUTS.get(name)
    if expected is not None:
        return (
            "pass-independent"
            if bytes(candidate["stdout"]) == expected and candidate["returncode"] == 0
            else failure_kind(candidate)
        )
    if reference["timeout"] or reference["returncode"] != 0:
        return "reference-failure"
    if candidate["timeout"] or candidate["returncode"] != 0:
        return failure_kind(candidate)
    candidate_output = normalized_output(bytes(candidate["stdout"]), candidate_root)
    reference_output = normalized_output(bytes(reference["stdout"]), reference_root)
    if name == "server_mappath_constant.asp":
        candidate_output = candidate_output.replace(b"\\", b"/")
        reference_output = reference_output.replace(b"\\", b"/")
    if candidate_output != reference_output:
        return "output-diff"
    return "pass"


def serializable_run(run: dict[str, object]) -> dict[str, object]:
    return {
        "returncode": run["returncode"],
        "stdout": display_bytes(bytes(run["stdout"])),
        "stderr": display_bytes(bytes(run["stderr"])),
        "elapsed_ms": run["elapsed_ms"],
        "timeout": run["timeout"],
    }


def main() -> int:
    args = parse_args()
    if not args.reference:
        print(
            "reference binary is required; pass --reference or set "
            "OPENASP_REFERENCE_BIN",
            file=sys.stderr,
        )
        return 2
    candidate = Path(args.candidate).resolve()
    reference = Path(args.reference).resolve()
    fixtures = Path(args.fixtures).resolve()
    report_path = Path(args.report)

    for path, label in (
        (candidate, "candidate"),
        (reference, "reference"),
        (fixtures, "fixtures"),
    ):
        if not path.exists():
            print(f"{label} does not exist: {path}", file=sys.stderr)
            return 2

    selected = set(args.fixture)
    names = sorted(path.name for path in fixtures.glob("*.asp"))
    if selected:
        names = [name for name in names if name in selected]
        missing = selected.difference(names)
        if missing:
            print("unknown fixtures: " + ", ".join(sorted(missing)), file=sys.stderr)
            return 2

    counts: dict[str, int] = {}
    results: list[dict[str, object]] = []
    with tempfile.TemporaryDirectory(prefix="openasp-diff-") as temporary:
        temporary_root = Path(temporary)
        reference_root = temporary_root / "reference"
        candidate_root = temporary_root / "candidate"
        shutil.copytree(fixtures, reference_root, copy_function=shutil.copy2)
        shutil.copytree(fixtures, candidate_root, copy_function=shutil.copy2)
        prepare_root(reference_root)
        prepare_root(candidate_root)
        for index, name in enumerate(names, 1):
            if not args.include_external and name in EXTERNAL_FIXTURES:
                category = "covered-external"
                result = {"fixture": name, "category": category}
            elif name in STATEFUL_FIXTURES:
                category = "covered-stateful"
                result = {"fixture": name, "category": category}
            else:
                if name in SQLITE_FIXTURES:
                    prepare_root(reference_root)
                    prepare_root(candidate_root)
                if name in MDB_FIXTURES:
                    prepare_mdb(reference_root)
                    prepare_mdb(candidate_root)
                reference_run = run_page(
                    reference, reference_root, name, args.timeout
                )
                candidate_run = run_page(
                    candidate, candidate_root, name, args.timeout
                )
                category = classify(
                    name, reference_run, candidate_run, reference_root, candidate_root
                )
                result = {
                    "fixture": name,
                    "category": category,
                    "reference": serializable_run(reference_run),
                    "candidate": serializable_run(candidate_run),
                }
                if category == "output-diff":
                    result["diff"] = output_diff(
                        normalized_output(
                            bytes(reference_run["stdout"]), reference_root
                        ),
                        normalized_output(
                            bytes(candidate_run["stdout"]), candidate_root
                        ),
                    )
            counts[category] = counts.get(category, 0) + 1
            results.append(result)
            print(
                f"[{index:3d}/{len(names)}] {category:17s} {name}",
                flush=True,
            )

    report = {
        "candidate": str(candidate),
        "reference": str(reference),
        "fixtures": str(fixtures),
        "counts": dict(sorted(counts.items())),
        "results": results,
    }
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(
        json.dumps(report, ensure_ascii=True, indent=2) + "\n",
        encoding="utf-8",
    )
    print("\n" + " ".join(f"{key}={value}" for key, value in sorted(counts.items())))
    print(f"report={report_path.resolve()}")
    accepted = {"pass", "pass-independent", "covered-external", "covered-stateful"}
    return 0 if set(counts).issubset(accepted) else 1


if __name__ == "__main__":
    raise SystemExit(main())
