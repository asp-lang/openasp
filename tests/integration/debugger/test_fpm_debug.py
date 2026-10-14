#!/usr/bin/env python3
# Copyright (c) 2026 https://asp-lang.org
# SPDX-License-Identifier: MIT

"""Exercise remote FPM debugger routing, auth, reconnect, and worker replacement."""

from __future__ import annotations

from pathlib import Path
import os
import select
import signal
import socket
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(next(parent for parent in Path(__file__).resolve().parents if (parent / "Makefile").is_file()) / "tests/integration/fastcgi"))
from test_fastcgi import start_server, transact


def worker_pids(debug_path: Path) -> list[int]:
    prefix = f"{debug_path.name}."
    return sorted(
        int(path.name[len(prefix):])
        for path in debug_path.parent.glob(f"{debug_path.name}.*")
        if path.name[len(prefix):].isdigit()
    )


def receive_line(client: socket.socket) -> bytes:
    result = bytearray()
    while not result.endswith(b"\n"):
        chunk = client.recv(1)
        if not chunk:
            raise RuntimeError("unexpected debugger EOF")
        result.extend(chunk)
    return bytes(result)


def attach_unix(path: Path, pid: int) -> socket.socket:
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.settimeout(5)
    client.connect(str(path))
    client.sendall(f"target {pid}\n".encode())
    assert receive_line(client) == f"attached {pid}\n".encode()
    return client


def attach_tcp(port: int, token: str, pid: int) -> socket.socket:
    client = socket.create_connection(("127.0.0.1", port), timeout=5)
    client.sendall(f"auth {token}\ntarget {pid}\n".encode())
    assert receive_line(client) == f"attached {pid}\n".encode()
    return client


def debug_request(socket_path: Path, fixtures: Path) -> bytes:
    return transact(
        socket_path,
        {
            "SCRIPT_FILENAME": str(fixtures / "debugger_basic.asp"),
            "SCRIPT_NAME": "/debugger_basic.asp",
            "REQUEST_METHOD": "GET",
            "QUERY_STRING": "",
            "SERVER_NAME": "localhost",
            "SERVER_PORT": "80",
        },
    )


def expect_stop_then_continue(clients: list[socket.socket], socket_path: Path, fixtures: Path) -> None:
    request = subprocess.Popen(
        [
            sys.executable,
            "-c",
            (
                "from pathlib import Path; import sys; "
                "sys.path.insert(0, sys.argv[1]); "
                "from test_fpm_debug import debug_request; "
                "print(debug_request(Path(sys.argv[2]), Path(sys.argv[3])).decode())"
            ),
            str(Path(__file__).resolve().parent),
            str(socket_path),
            str(fixtures),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        readable, _, _ = select.select(clients, [], [], 5)
        assert readable, "worker did not reach its debugger stop"
        client = readable[0]
        stopped = client.recv(8192)
        assert b"[Inferior process " in stopped and b"pc=" in stopped, stopped
        client.sendall(b"continue\n")
        output, error = request.communicate(timeout=5)
        assert request.returncode == 0, error
        assert "41" in output, output
    finally:
        if request.poll() is None:
            request.kill()
            request.wait(timeout=5)


def main() -> int:
    binary = Path(sys.argv[1]).resolve()
    fixtures = Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(prefix="openasp-fpm-debug-") as directory:
        temporary = Path(directory)
        socket_path = temporary / "fastcgi.sock"
        debug_path = temporary / "debug.sock"
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
            probe.bind(("127.0.0.1", 0))
            tcp_port = probe.getsockname()[1]
        command = [
            str(binary),
            "--root",
            str(fixtures),
            "--unix-socket",
            str(socket_path),
            "--workers",
            "2",
            "--debug-enable=on",
            "--debug-unix-socket",
            str(debug_path),
            "--debug-tcp-host",
            "127.0.0.1",
            "--debug-tcp-port",
            str(tcp_port),
            "--debug-token",
            "debug-test-token",
            "--opcache-dir",
            str(temporary / "cache"),
        ]
        server = start_server(command, socket_path)
        try:
            for _ in range(100):
                pids = worker_pids(debug_path)
                if len(pids) == 2 and debug_path.exists():
                    break
                time.sleep(0.01)
            assert len(pids) == 2, pids

            rejected = socket.create_connection(("127.0.0.1", tcp_port), timeout=5)
            rejected.sendall(f"auth wrong\ntarget {pids[0]}\n".encode())
            assert receive_line(rejected).startswith(b"error ")
            rejected.close()

            tcp_client = attach_tcp(tcp_port, "debug-test-token", pids[1])
            tcp_client.close()
            unix_clients = [attach_unix(debug_path, pid) for pid in pids]
            expect_stop_then_continue(unix_clients, socket_path, fixtures)
            for unix_client in unix_clients:
                unix_client.close()

            os.kill(pids[0], signal.SIGKILL)
            deadline = time.monotonic() + 5
            replaced = pids
            while time.monotonic() < deadline:
                replaced = worker_pids(debug_path)
                if len(replaced) == 2 and pids[0] not in replaced:
                    break
                time.sleep(0.05)
            assert len(replaced) == 2 and pids[0] not in replaced, (pids, replaced)
            replacement_client = attach_unix(debug_path, next(pid for pid in replaced if pid not in pids))
            replacement_client.close()

            assert b"\r\n\r\n41" in debug_request(socket_path, fixtures)
            for _ in range(16):
                transient = attach_unix(debug_path, replaced[0])
                transient.close()
                assert b"\r\n\r\n41" in debug_request(socket_path, fixtures)
            assert server.poll() is None
        finally:
            if server.poll() is None:
                server.terminate()
                server.wait(timeout=5)
    print("fpm debug tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
