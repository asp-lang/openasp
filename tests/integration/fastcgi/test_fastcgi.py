#!/usr/bin/env python3
# Copyright (c) 2026 https://asp-lang.org
# SPDX-License-Identifier: MIT

"""Exercise one complete FastCGI request over a Unix domain socket."""

from __future__ import annotations

from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.parse


def record(kind: int, request_id: int, content: bytes) -> bytes:
    return struct.pack("!BBHHBB", 1, kind, request_id, len(content), 0, 0) + content


def parameter(name: str, value: str) -> bytes:
    name_bytes = name.encode()
    value_bytes = value.encode()

    def length(value_length: int) -> bytes:
        if value_length < 128:
            return bytes([value_length])
        return struct.pack("!I", value_length | 0x80000000)

    return (
        length(len(name_bytes))
        + length(len(value_bytes))
        + name_bytes
        + value_bytes
    )


def receive_exact(client: socket.socket, length: int) -> bytes:
    result = bytearray()
    while len(result) < length:
        chunk = client.recv(length - len(result))
        if not chunk:
            raise RuntimeError("unexpected FastCGI EOF")
        result.extend(chunk)
    return bytes(result)

def transact_records(
    socket_path: Path, params: dict[str, str], body: bytes = b""
) -> list[tuple[int, bytes]]:
    encoded = b"".join(
        parameter(name, value) for name, value in params.items()
    )
    payload = (
        record(1, 1, struct.pack("!HB5x", 1, 0))
        + record(4, 1, encoded)
        + record(4, 1, b"")
        + (record(5, 1, body) if body else b"")
        + record(5, 1, b"")
    )
    records = []
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(5)
        client.connect(str(socket_path))
        client.sendall(payload)
        while True:
            header = receive_exact(client, 8)
            _, kind, request_id, content_length, padding, _ = (
                struct.unpack("!BBHHBB", header)
            )
            assert request_id == 1
            content = receive_exact(client, content_length)
            if padding:
                receive_exact(client, padding)
            records.append((kind, content))
            if kind == 3:
                break
    return records


def transact(
    socket_path: Path, params: dict[str, str], body: bytes = b""
) -> bytes:
    return b"".join(
        content
        for kind, content in transact_records(socket_path, params, body)
        if kind == 6
    )


def start_server(
    command: list[str],
    socket_path: Path,
    *,
    stdout: object = subprocess.PIPE,
    stderr: object = subprocess.PIPE,
) -> subprocess.Popen:
    server = subprocess.Popen(
        command, stdout=stdout, stderr=stderr
    )
    for _ in range(100):
        if socket_path.exists():
            return server
        if server.poll() is not None:
            if server.stderr is not None:
                raise RuntimeError(server.stderr.read().decode())
            raise RuntimeError(f"FastCGI exited with status {server.returncode}")
        time.sleep(0.01)
    server.terminate()
    server.wait(timeout=5)
    raise RuntimeError("FastCGI socket was not created")


def limited_request(
    binary: Path,
    root: Path,
    temporary: Path,
    name: str,
    options: list[str],
    params: dict[str, str],
    body: bytes = b"",
) -> bytes:
    socket_path = temporary / f"{name}.sock"
    command = [
        str(binary),
        "--root",
        str(root),
        "--unix-socket",
        str(socket_path),
        "--max-accepts",
        "1",
        "--workers",
        "1",
        "--opcache-dir",
        str(temporary / f"{name}-cache"),
        *options,
    ]
    server = start_server(command, socket_path)
    try:
        output = transact(socket_path, params, body)
        assert server.wait(timeout=5) == 0
        return output
    finally:
        if server.poll() is None:
            server.terminate()
            server.wait(timeout=5)


def main() -> int:
    binary = Path(sys.argv[1]).resolve()
    fixtures = Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(prefix="openasp-fcgi-") as temporary:
        socket_path = Path(temporary) / "openasp.sock"
        command = [
            str(binary),
            "--root",
            str(fixtures),
            "--unix-socket",
            str(socket_path),
            "--max-accepts",
            "4",
            "--workers",
            "4",
            "--opcache-dir",
            str(Path(temporary) / "cache"),
        ]
        server = start_server(command, socket_path)
        try:
            base_params = {
                "SCRIPT_FILENAME": str(fixtures / "index.asp"),
                "SCRIPT_NAME": "/index.asp",
                "REQUEST_METHOD": "GET",
                "QUERY_STRING": "name=FastCGI",
                "SERVER_NAME": "localhost",
                "SERVER_PORT": "80",
            }
            output = transact(socket_path, base_params)
            assert b"Status: 200 OK" in output
            assert b"<div>hello=FastCGI</div>" in output

            case_params = {
                "SCRIPT_FILENAME": str(fixtures / "MINIMAL_PROFILE.ASP"),
                "SCRIPT_NAME": "/MINIMAL_PROFILE.ASP",
                "REQUEST_METHOD": "GET",
                "QUERY_STRING": "",
                "SERVER_NAME": "localhost",
                "SERVER_PORT": "80",
            }
            case_resolved = limited_request(
                binary, fixtures, Path(temporary), "case-path", [], case_params
            )
            assert b"Status: 200 OK" in case_resolved
            assert case_resolved.endswith(b"\r\n\r\n42\n")

            missing_params = dict(case_params)
            missing_params["SCRIPT_FILENAME"] = str(
                fixtures / "does-not-exist.asp"
            )
            missing = limited_request(
                binary, fixtures, Path(temporary), "missing-path", [], missing_params
            )
            assert b"Status: 404 Not Found" in missing
            assert missing.endswith(b"\r\n\r\nNot Found\n")

            params = dict(base_params)
            params["SCRIPT_FILENAME"] = str(
                fixtures / "set_session_complex.asp"
            )
            params["QUERY_STRING"] = "color=blue"
            stored = transact(socket_path, params)
            assert b"\r\n\r\nstored" in stored
            cookie_header = next(
                line for line in stored.split(b"\r\n")
                if line.startswith(b"Set-Cookie: OPENASPID=")
            )
            session_cookie = cookie_header.split(b";", 1)[0].split(b": ", 1)[1]

            params["SCRIPT_FILENAME"] = str(
                fixtures / "get_session_complex.asp"
            )
            params["QUERY_STRING"] = ""
            params["HTTP_COOKIE"] = session_cookie.decode()
            loaded = transact(socket_path, params)
            assert b"\r\n\r\nblue:two:33:blue:one" in loaded

            server.terminate()
            assert server.wait(timeout=5) == 0
            server = start_server(command, socket_path)
            loaded_after_restart = transact(socket_path, params)
            assert b"\r\n\r\nblue:two:33:blue:one" in loaded_after_restart

            params["SCRIPT_FILENAME"] = str(
                fixtures / "store_user_class.asp"
            )
            stored_class = transact(socket_path, params)
            assert b"\r\n\r\nstored" in stored_class

            params["SCRIPT_FILENAME"] = str(
                fixtures / "load_user_class.asp"
            )
            loaded_class = transact(socket_path, params)
            if b"\r\n\r\npersisted|persisted" not in loaded_class:
                server.wait(timeout=5)
                raise AssertionError(
                    (loaded_class, server.stderr.read().decode())
                )
            cached_class = transact(socket_path, params)
            assert b"\r\n\r\npersisted|persisted" in cached_class
            assert server.wait(timeout=5) == 0

            compatibility_socket = Path(temporary) / "compatibility.sock"
            compatibility_root = next(parent for parent in Path(__file__).resolve().parents if (parent / "Makefile").is_file()) / "tests/fixtures/common"
            compatibility_command = [
                str(binary),
                "--root",
                str(compatibility_root),
                "--unix-socket",
                str(compatibility_socket),
                "--max-accepts",
                "4",
                "--workers",
                "1",
                "--opcache-dir",
                str(Path(temporary) / "compatibility-cache"),
            ]
            server = start_server(compatibility_command, compatibility_socket)
            compatibility_params = dict(base_params)
            compatibility_params["SCRIPT_FILENAME"] = str(
                compatibility_root / "object_array_member.asp"
            )
            compatibility_params["SCRIPT_NAME"] = "/object_array_member.asp"
            compatibility_params["QUERY_STRING"] = ""
            array_member = transact(
                compatibility_socket, compatibility_params
            )
            assert b"\r\n\r\none" in array_member, array_member

            compatibility_params["SCRIPT_FILENAME"] = str(
                compatibility_root / "server_transfer.asp"
            )
            compatibility_params["SCRIPT_NAME"] = "/server_transfer.asp"
            compatibility_params["QUERY_STRING"] = "value=ok"
            transferred = transact(
                compatibility_socket, compatibility_params
            )
            assert b"\r\n\r\ntransferred:ok" in transferred, transferred
            assert b"discarded-before-transfer" not in transferred
            assert b"unreachable-after-transfer" not in transferred

            compatibility_params["SCRIPT_FILENAME"] = str(
                compatibility_root / "select_case_metadata.asp"
            )
            compatibility_params["SCRIPT_NAME"] = "/select_case_metadata.asp"
            compatibility_params["QUERY_STRING"] = ""
            selected = transact(
                compatibility_socket, compatibility_params
            )
            assert b"\r\n\r\nthird" in selected, selected

            compatibility_params["SCRIPT_FILENAME"] = str(
                compatibility_root / "legacy_compatibility.asp"
            )
            compatibility_params["SCRIPT_NAME"] = "/legacy_compatibility.asp"
            compatibility_params["HTTP_USER_AGENT"] = "OpenASP-Test"
            legacy = transact(compatibility_socket, compatibility_params)
            assert b"X-OpenASP-Test: ready" in legacy, legacy
            assert b"\r\n\r\n[assigned]|[ok]|leftright|OpenASP-Test|42" in legacy, legacy
            assert server.wait(timeout=5) == 0

            sqlite_socket = Path(temporary) / "sqlite.sock"
            sqlite_root = next(parent for parent in Path(__file__).resolve().parents if (parent / "Makefile").is_file()) / "tests/fixtures/common"
            sqlite_command = [
                str(binary),
                "--root",
                str(sqlite_root),
                "--unix-socket",
                str(sqlite_socket),
                "--max-accepts",
                "1",
                "--workers",
                "1",
                "--opcache-dir",
                str(Path(temporary) / "sqlite-cache"),
                "--opcache-validate-timestamps=off",
            ]
            server = start_server(sqlite_command, sqlite_socket)
            sqlite_params = dict(base_params)
            sqlite_params["SCRIPT_FILENAME"] = str(
                sqlite_root / "sqlite_pool_rollback.asp"
            )
            sqlite_params["SCRIPT_NAME"] = "/sqlite_pool_rollback.asp"
            sqlite_params["QUERY_STRING"] = urllib.parse.urlencode(
                {"db": str(Path(temporary) / "pool.sqlite")}
            )
            sqlite_output = transact(sqlite_socket, sqlite_params)
            assert b"\r\n\r\n0" in sqlite_output, sqlite_output
            assert b"Set-Cookie: OPENASPID=" not in sqlite_output
            assert server.wait(timeout=5) == 0

            stream_socket = Path(temporary) / "stream.sock"
            stream_root = next(parent for parent in Path(__file__).resolve().parents if (parent / "Makefile").is_file()) / "tests/fixtures/common"
            stream_command = [
                str(binary),
                "--root",
                str(stream_root),
                "--unix-socket",
                str(stream_socket),
                "--max-accepts",
                "1",
                "--workers",
                "1",
                "--opcache-dir",
                str(Path(temporary) / "stream-cache"),
            ]
            server = start_server(stream_command, stream_socket)
            stream_params = dict(base_params)
            stream_params["SCRIPT_FILENAME"] = str(
                stream_root / "streaming_response.asp"
            )
            stream_params["SCRIPT_NAME"] = "/streaming_response.asp"
            stream_params["QUERY_STRING"] = ""
            stream_records = transact_records(stream_socket, stream_params)
            stdout_records = [
                content for kind, content in stream_records
                if kind == 6 and content
            ]
            assert len(stdout_records) >= 3, stdout_records
            streamed = b"".join(stdout_records)
            headers, body = streamed.split(b"\r\n\r\n", 1)
            assert b"Status: 200 OK" in headers
            assert body == b"AB\n", body
            assert server.wait(timeout=5) == 0

            for option, value in (
                ("--port", "65536"),
                ("--workers", "1x"),
                ("--max-requests", "-1"),
                ("--opcache-memory", "64MiB"),
                ("--memory-limit", "0"),
            ):
                invalid = subprocess.run(
                    [str(binary), option, value],
                    capture_output=True,
                    check=False,
                )
                assert invalid.returncode == 2, (option, invalid)

            limit_params = dict(base_params)
            limit_params["SCRIPT_FILENAME"] = str(
                compatibility_root / "response_limit.asp"
            )
            limit_params["SCRIPT_NAME"] = "/response_limit.asp"
            limit_params["QUERY_STRING"] = ""

            params_rejected = limited_request(
                binary,
                compatibility_root,
                Path(temporary),
                "params-limit",
                ["--max-params-bytes", "32"],
                limit_params,
            )
            assert b"Status: 413 Content Too Large" in params_rejected

            body_rejected = limited_request(
                binary,
                compatibility_root,
                Path(temporary),
                "body-limit",
                ["--max-body-bytes", "4"],
                limit_params,
                b"12345",
            )
            assert b"Status: 413 Content Too Large" in body_rejected

            request_rejected = limited_request(
                binary,
                compatibility_root,
                Path(temporary),
                "request-limit",
                [
                    "--max-params-bytes",
                    "0",
                    "--max-body-bytes",
                    "0",
                    "--max-request-bytes",
                    "64",
                ],
                limit_params,
            )
            assert b"Status: 413 Content Too Large" in request_rejected

            response_rejected = limited_request(
                binary,
                compatibility_root,
                Path(temporary),
                "response-limit",
                ["--max-buffered-response-bytes", "8"],
                limit_params,
            )
            assert b"Status: 500 Internal Server Error" in response_rejected

            memory_socket = Path(temporary) / "memory-limit.sock"
            memory_command = [
                str(binary),
                "--root",
                str(compatibility_root),
                "--unix-socket",
                str(memory_socket),
                "--max-accepts",
                "2",
                "--workers",
                "1",
                "--memory-limit",
                "512K",
                "--opcache-dir",
                str(Path(temporary) / "memory-limit-cache"),
            ]
            server = start_server(memory_command, memory_socket)
            memory_params = dict(limit_params)
            memory_params["SCRIPT_FILENAME"] = str(
                compatibility_root / "memory_limit.asp"
            )
            memory_params["SCRIPT_NAME"] = "/memory_limit.asp"
            memory_rejected = transact(memory_socket, memory_params)
            assert b"Status: 500 Internal Server Error" in memory_rejected
            isolated_params = dict(limit_params)
            isolated_params["SCRIPT_FILENAME"] = str(
                compatibility_root / "minimal_profile.asp"
            )
            isolated_params["SCRIPT_NAME"] = "/minimal_profile.asp"
            isolated = transact(memory_socket, isolated_params)
            assert b"\r\n\r\n42" in isolated
            assert server.wait(timeout=5) == 0

            header_params = dict(limit_params)
            header_params["SCRIPT_FILENAME"] = str(
                compatibility_root / "header_limit.asp"
            )
            header_params["SCRIPT_NAME"] = "/header_limit.asp"
            header_rejected = limited_request(
                binary,
                compatibility_root,
                Path(temporary),
                "header-limit",
                ["--max-response-headers", "1"],
                header_params,
            )
            assert b"Status: 500 Internal Server Error" in header_rejected

            cookie_params = dict(limit_params)
            cookie_params["SCRIPT_FILENAME"] = str(
                compatibility_root / "cookie_limit.asp"
            )
            cookie_params["SCRIPT_NAME"] = "/cookie_limit.asp"
            cookie_rejected = limited_request(
                binary,
                compatibility_root,
                Path(temporary),
                "cookie-limit",
                ["--max-response-header-bytes", "32"],
                cookie_params,
            )
            assert b"Status: 500 Internal Server Error" in cookie_rejected

            deadline_params = dict(limit_params)
            deadline_params["SCRIPT_FILENAME"] = str(
                compatibility_root / "deadline_loop.asp"
            )
            deadline_params["SCRIPT_NAME"] = "/deadline_loop.asp"
            timed_out = limited_request(
                binary,
                compatibility_root,
                Path(temporary),
                "deadline",
                ["--request-timeout-ms", "20"],
                deadline_params,
            )
            assert b"Status: 504 Gateway Timeout" in timed_out

            timeout_socket = Path(temporary) / "client-timeout.sock"
            timeout_command = [
                str(binary),
                "--root",
                str(compatibility_root),
                "--unix-socket",
                str(timeout_socket),
                "--max-accepts",
                "1",
                "--client-timeout-ms",
                "100",
            ]
            server = start_server(timeout_command, timeout_socket)
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                client.settimeout(2)
                client.connect(str(timeout_socket))
                time.sleep(0.25)
                assert client.recv(1) == b""
            return server.wait(timeout=5)
        finally:
            if server.poll() is None:
                server.terminate()
                server.wait(timeout=5)


if __name__ == "__main__":
    raise SystemExit(main())
