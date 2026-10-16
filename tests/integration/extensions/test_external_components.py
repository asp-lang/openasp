#!/usr/bin/env python3
# Copyright (c) 2026 https://asp-lang.org
# SPDX-License-Identifier: MIT

"""Run all fixtures that require local services or privileged components."""

from __future__ import annotations

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import importlib.util
from pathlib import Path
import os
import re
import shutil
import socket
import socketserver
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse

sys.path.insert(0, str(next(parent for parent in Path(__file__).resolve().parents if (parent / "Makefile").is_file()) / "tests/integration/fastcgi"))
from test_fastcgi import start_server, transact


ROOT = next(parent for parent in Path(__file__).resolve().parents if (parent / "Makefile").is_file())
OPENASP = ROOT / "build/bin/openasp"
OPENASP_FPM = ROOT / "build/bin/openasp-fpm"
FIXTURES = ROOT / "tests/fixtures/reference"
OWNED_REDIS: subprocess.Popen[bytes] | None = None


def run_fixture(name: str, query: dict[str, object] | None = None,
                *options: str) -> subprocess.CompletedProcess[str]:
    command = [
        str(OPENASP), str(FIXTURES / name), "--root", str(FIXTURES),
    ]
    if query is not None:
        command.extend(["--query", urllib.parse.urlencode(query)])
    command.extend(options)
    return subprocess.run(
        command, capture_output=True, text=True, timeout=25,
        env={**os.environ, "LC_ALL": "C"},
    )


def expect(name: str, expected: str, query: dict[str, object] | None = None,
           *options: str) -> None:
    result = run_fixture(name, query, *options)
    assert result.returncode == 0, f"{name}: {result.stderr}"
    assert result.stdout.strip() == expected, (
        f"{name}: expected {expected!r}, got {result.stdout.strip()!r}"
    )
    print(f"PASS {name}")


def tcp_available(port: int) -> bool:
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=0.5):
            return True
    except OSError:
        return False


def ensure_redis() -> int:
    global OWNED_REDIS
    port = int(os.environ.get("OPENASP_REDIS_PORT", "6379"))
    if not tcp_available(port):
        OWNED_REDIS = subprocess.Popen(
            [
                "redis-server", "--bind", "127.0.0.1", "--port", str(port),
                "--save", "", "--appendonly", "no", "--daemonize", "no",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and not tcp_available(port):
            if OWNED_REDIS.poll() is not None:
                stderr = OWNED_REDIS.stderr.read().decode(errors="replace") if OWNED_REDIS.stderr else ""
                raise RuntimeError(f"redis-server exited during startup: {stderr}")
            time.sleep(0.05)
    assert tcp_available(port), f"Redis is unavailable on port {port}"
    return port


def mysql_port() -> int:
    configured = os.environ.get("OPENASP_MYSQL_PORT")
    ports = [int(configured)] if configured else [13316, 3306]
    for port in ports:
        if tcp_available(port):
            return port
    if subprocess.run(
        ["sh", "-c", "command -v brew >/dev/null && brew services start mysql"],
        capture_output=True, text=True,
    ).returncode == 0:
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            for port in ports:
                if tcp_available(port):
                    return port
            time.sleep(0.2)
    raise AssertionError("MySQL is unavailable on ports 13316 and 3306")


def check_reference_backend_harness() -> None:
    path = ROOT / "tests/integration/extensions/reference_backend_checks.py"
    spec = importlib.util.spec_from_file_location("openasp_backend_checks", path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.OPENASP = OPENASP
    module.FIXTURES = FIXTURES
    original_popen = module.subprocess.Popen

    def run_reference_fixture(
        name: str, *options: str
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                str(OPENASP),
                str(FIXTURES / name),
                "--root",
                str(FIXTURES),
                *options,
            ],
            capture_output=True,
            text=True,
            timeout=20,
        )

    def compatible_popen(
        command: object, *args: object, **kwargs: object
    ) -> subprocess.Popen[bytes]:
        if isinstance(command, list):
            filtered: list[object] = []
            index = 0
            while index < len(command):
                if command[index] == "--log-file":
                    index += 2
                    continue
                filtered.append(command[index])
                index += 1
            command = filtered
        return original_popen(command, *args, **kwargs)

    module.run_fixture = run_reference_fixture
    module.subprocess.Popen = compatible_popen
    try:
        module.main()
    finally:
        module.subprocess.Popen = original_popen
    for name in (
        "component_backend_services.asp",
        "component_process.asp",
        "component_websocket_client.asp",
        "component_websocket_server.asp",
        "component_memcache.asp",
        "component_https.asp",
        "component_websocket_wss.asp",
    ):
        print(f"PASS {name}")


class HttpHandler(BaseHTTPRequestHandler):
    def do_GET(self) -> None:
        if self.path != "/status":
            assert self.headers.get("X-Component-Test") == "yes"
        body = b"http-ok"
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format: str, *args: object) -> None:
        pass


def check_http() -> None:
    server = ThreadingHTTPServer(("127.0.0.1", 0), HttpHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        expect(
            "component_http.asp", "4:200:http-ok:text/plain",
            {"url": f"http://127.0.0.1:{server.server_port}/fixture"},
        )
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)


class HttpMultiHandler(BaseHTTPRequestHandler):
    lock = threading.Lock()
    active = 0
    max_active = 0

    def handle_parallel(self, expected_body: bytes | None) -> None:
        if expected_body is not None:
            length = int(self.headers.get("Content-Length", "0"))
            assert self.rfile.read(length) == expected_body
            assert self.headers.get("Content-Type") == "application/json"
        with self.lock:
            type(self).active += 1
            type(self).max_active = max(type(self).max_active, type(self).active)
        try:
            time.sleep(0.6)
            body = b"post-ok" if expected_body is not None else b"get-ok"
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        finally:
            with self.lock:
                type(self).active -= 1

    def do_GET(self) -> None:
        assert self.path == "/parallel/first"
        self.handle_parallel(None)

    def do_POST(self) -> None:
        assert self.path == "/parallel/second"
        self.handle_parallel(b'{"id":123}')

    def log_message(self, format: str, *args: object) -> None:
        pass


def check_http_multi() -> None:
    HttpMultiHandler.active = 0
    HttpMultiHandler.max_active = 0
    server = ThreadingHTTPServer(("127.0.0.1", 0), HttpMultiHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        expect(
            "component_http_multi.asp",
            "3:2:5000|200:True:0:get-ok|200:True:0:post-ok|True:7",
            {"port": server.server_port},
        )
        assert HttpMultiHandler.max_active == 2, (
            f"expected two concurrent HTTP requests, got {HttpMultiHandler.max_active}"
        )
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)


class RedisMultiHandler(socketserver.BaseRequestHandler):
    def handle(self) -> None:
        stream = self.request.makefile("rb")
        try:
            for _ in range(self.server.command_count):
                header = stream.readline()
                assert header.startswith(b"*") and header.endswith(b"\r\n"), header
                argument_count = int(header[1:-2])
                for _ in range(argument_count):
                    bulk = stream.readline()
                    assert bulk.startswith(b"$") and bulk.endswith(b"\r\n"), bulk
                    length = int(bulk[1:-2])
                    assert len(stream.read(length)) == length
                    assert stream.read(2) == b"\r\n"
            tracker = self.server.tracker
            with tracker["lock"]:
                tracker["active"] += 1
                tracker["maximum"] = max(tracker["maximum"], tracker["active"])
            try:
                time.sleep(1.0)
                self.request.sendall(self.server.response)
            finally:
                with tracker["lock"]:
                    tracker["active"] -= 1
        finally:
            stream.close()


class RedisMultiServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def check_redis_multi() -> None:
    tracker = {"lock": threading.Lock(), "active": 0, "maximum": 0}
    servers = []
    threads = []
    specifications = [
        (2, b"+PONG\r\n$5\r\nalpha\r\n"),
        (
            3,
            b"-WRONGTYPE Operation against a key holding the wrong kind of value\r\n"
            b"$2\r\n42\r\n*2\r\n$3\r\none\r\n$3\r\ntwo\r\n",
        ),
    ]
    for command_count, response in specifications:
        server = RedisMultiServer(("127.0.0.1", 0), RedisMultiHandler)
        server.command_count = command_count
        server.response = response
        server.tracker = tracker
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        servers.append(server)
        threads.append(thread)
    unused = socket.socket()
    unused.bind(("127.0.0.1", 0))
    failed_port = unused.getsockname()[1]
    unused.close()
    started = time.monotonic()
    try:
        expect(
            "component_redis_multi.asp",
            "3:6:2:5000:4194304:16777216|PONG:alpha|3000:"
            "WRONGTYPE Operation against a key holding the wrong kind of value:"
            "42:one,two|True:True:False",
            {
                "port_a": servers[0].server_address[1],
                "port_b": servers[1].server_address[1],
                "failed_port": failed_port,
            },
        )
        elapsed = time.monotonic() - started
        assert tracker["maximum"] == 2, tracker
        assert elapsed < 2.2, f"Redis endpoints were not concurrent: {elapsed:.3f}s"
    finally:
        for server in servers:
            server.shutdown()
            server.server_close()
        for thread in threads:
            thread.join(timeout=2)


class MemcacheMultiHandler(socketserver.BaseRequestHandler):
    def handle(self) -> None:
        stream = self.request.makefile("rb")
        try:
            for _ in range(self.server.command_count):
                line = stream.readline()
                assert line.endswith(b"\r\n"), line
                fields = line[:-2].split()
                assert fields, line
                if fields[0].lower() in {b"set", b"add", b"replace", b"append", b"prepend", b"cas"}:
                    length = int(fields[4])
                    assert len(stream.read(length)) == length
                    assert stream.read(2) == b"\r\n"
            tracker = self.server.tracker
            with tracker["lock"]:
                tracker["active"] += 1
                tracker["maximum"] = max(tracker["maximum"], tracker["active"])
            try:
                time.sleep(1.0)
                self.request.sendall(self.server.response)
            finally:
                with tracker["lock"]:
                    tracker["active"] -= 1
        finally:
            stream.close()


class MemcacheMultiServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def check_memcache_multi() -> None:
    tracker = {"lock": threading.Lock(), "active": 0, "maximum": 0}
    servers = []
    threads = []
    specifications = [
        (
            4,
            b"VALUE first 7 5\r\nalpha\r\nEND\r\n"
            b"VALUE second 8 4 99\r\nbeta\r\nEND\r\n"
            b"STORED\r\nNOT_FOUND\r\n",
        ),
        (
            5,
            b"42\r\nTOUCHED\r\nVERSION 1.6.21\r\nOK\r\n"
            b"SERVER_ERROR backend failure\r\n",
        ),
    ]
    for command_count, response in specifications:
        server = MemcacheMultiServer(("127.0.0.1", 0), MemcacheMultiHandler)
        server.command_count = command_count
        server.response = response
        server.tracker = tracker
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        servers.append(server)
        threads.append(thread)
    unused = socket.socket()
    unused.bind(("127.0.0.1", 0))
    failed_port = unused.getsockname()[1]
    unused.close()
    started = time.monotonic()
    try:
        expect(
            "component_memcache_multi.asp",
            "3:10:2:5000:4194304:16777216|alpha:7:0|beta:8:99|True:False"
            "|42:True:1.6.21:True|13000:SERVER_ERROR backend failure|True:True:False",
            {
                "port_a": servers[0].server_address[1],
                "port_b": servers[1].server_address[1],
                "failed_port": failed_port,
            },
        )
        elapsed = time.monotonic() - started
        assert tracker["maximum"] == 2, tracker
        assert elapsed < 2.2, f"Memcache endpoints were not concurrent: {elapsed:.3f}s"
    finally:
        for server in servers:
            server.shutdown()
            server.server_close()
        for thread in threads:
            thread.join(timeout=2)


def check_memcache_multi_limits() -> None:
    tracker = {"lock": threading.Lock(), "active": 0, "maximum": 0}
    server = MemcacheMultiServer(("127.0.0.1", 0), MemcacheMultiHandler)
    server.command_count = 1
    server.response = b"END\r\n"
    server.tracker = tracker
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        expect(
            "component_memcache_multi_limits.asp",
            "7:True:True:True",
            {"port": server.server_address[1]},
        )
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)


def check_http_status_properties() -> None:
    server = ThreadingHTTPServer(("127.0.0.1", 0), HttpHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        fixture = ROOT / "tests/conformance/cases/http_status_properties.asp"
        result = subprocess.run(
            [
                str(OPENASP), str(fixture), "--root", str(fixture.parent),
                "--query", urllib.parse.urlencode({"port": server.server_port}),
            ],
            capture_output=True, text=True, timeout=25,
            env={**os.environ, "LC_ALL": "C"},
        )
        assert result.returncode == 0, result.stderr
        assert result.stdout.strip() == "200|4|http-ok|text/plain", result.stdout
        print("PASS http_status_properties.asp")
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)


def check_http_tls_configuration_properties() -> None:
    fixture = ROOT / "tests/conformance/cases/http_tls_configuration_properties.asp"
    result = subprocess.run(
        [str(OPENASP), str(fixture), "--root", str(fixture.parent)],
        capture_output=True, text=True, timeout=25, env={**os.environ, "LC_ALL": "C"},
    )
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "6789|54321|False|/tmp/test-ca.pem", result.stdout
    print("PASS http_tls_configuration_properties.asp")


def check_memcache_configuration_properties() -> None:
    fixture = ROOT / "tests/conformance/cases/memcache_configuration_properties.asp"
    result = subprocess.run(
        [str(OPENASP), str(fixture), "--root", str(fixture.parent)],
        capture_output=True, text=True, timeout=25, env={**os.environ, "LC_ALL": "C"},
    )
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "example.test|12345|text|6789|True|False|0|0|0", result.stdout
    print("PASS memcache_configuration_properties.asp")


def echo_stream(listener: socket.socket) -> None:
    connection, _ = listener.accept()
    with connection:
        connection.sendall(connection.recv(4096))
    listener.close()


def echo_datagram(peer: socket.socket) -> None:
    data, address = peer.recvfrom(4096)
    peer.sendto(data, address)
    peer.close()


def check_socket_client() -> None:
    for protocol in ("tcp", "unix", "udp"):
        with tempfile.TemporaryDirectory(prefix="openasp-socket-") as directory:
            if protocol == "tcp":
                peer = socket.create_server(("127.0.0.1", 0))
                query = {"protocol": protocol, "port": peer.getsockname()[1]}
                target = echo_stream
            elif protocol == "unix":
                path = str(Path(directory) / "peer.sock")
                peer = socket.socket(socket.AF_UNIX)
                peer.bind(path)
                peer.listen()
                query = {"protocol": protocol, "path": path}
                target = echo_stream
            else:
                peer = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                peer.bind(("127.0.0.1", 0))
                query = {"protocol": protocol, "port": peer.getsockname()[1]}
                target = echo_datagram
            thread = threading.Thread(target=target, args=(peer,), daemon=True)
            thread.start()
            expect(
                "component_socket_client.asp",
                f"{protocol}:1:13:socket-client:0", query,
            )
            thread.join(timeout=2)
            assert not thread.is_alive()


def unused_tcp_port() -> int:
    probe = socket.socket()
    probe.bind(("127.0.0.1", 0))
    port = probe.getsockname()[1]
    probe.close()
    return port


def check_socket_listener() -> None:
    for protocol in ("tcp", "unix"):
        with tempfile.TemporaryDirectory(prefix="openasp-listener-") as directory:
            if protocol == "tcp":
                address: tuple[str, int] | str = ("127.0.0.1", unused_tcp_port())
                query = {"protocol": protocol, "port": address[1]}
                family = socket.AF_INET
            else:
                address = str(Path(directory) / "listener.sock")
                query = {"protocol": protocol, "path": address}
                family = socket.AF_UNIX
            command = [
                str(OPENASP), str(FIXTURES / "component_socket_listener.asp"),
                "--root", str(FIXTURES), "--query",
                urllib.parse.urlencode(query),
            ]
            process = subprocess.Popen(
                command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True,
            )
            try:
                client = None
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    try:
                        client = socket.socket(family)
                        client.connect(address)
                        break
                    except OSError:
                        if client:
                            client.close()
                        time.sleep(0.02)
                assert client is not None, f"{protocol} listener did not start"
                with client:
                    client.sendall(b"listener-data")
                    assert client.recv(4096) == b"echo:listener-data"
                stdout, stderr = process.communicate(timeout=10)
                assert process.returncode == 0, stderr
                assert stdout.strip() == f"{protocol}:3:1:listener-data:0:0"
            finally:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=2)
    print("PASS component_socket_listener.asp")


def check_socket_udp() -> None:
    peer = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    peer.bind(("127.0.0.1", 0))
    port = peer.getsockname()[1]
    thread = threading.Thread(target=echo_datagram, args=(peer,), daemon=True)
    thread.start()
    result = run_fixture("component_socket_udp.asp", {"port": port})
    assert result.returncode == 0, result.stderr
    pattern = rf"udp:2:\d+:10:socket-udp:127\.0\.0\.1:{port}:0"
    assert re.fullmatch(pattern, result.stdout.strip()), result.stdout
    thread.join(timeout=2)
    assert not thread.is_alive()
    print("PASS component_socket_udp.asp")


def check_redis_and_mysql() -> None:
    redis_port = ensure_redis()
    check_redis_configuration_properties(redis_port)
    expect(
        "component_redis_multi_real.asp",
        "0:2:one,two:OK:42",
        {"port": redis_port},
    )
    expect(
        "component_redis.asp",
        "1:OK:hello:10:9:1:ttl:1:egret:name=egret:1:2:3:"
        "zero,one:zero:two:1:member:PONG:0",
        {"port": redis_port},
    )
    port = mysql_port()
    expectations = {
        "component_mysql.asp": "1:plain:41:bound:42:0",
        "component_mysql_datetime_rewrite.asp":
            "2026-09-24 05:06:07|release 9/24/2026 note|2/29/2025|"
            "2024-02-29 23:59:58|prepared",
        "component_mysql_prepare.asp": "bound",
        "component_mysql_provider_fallback.asp":
            "mysql-fallback:mysql-default",
    }
    for name, output in expectations.items():
        expect(name, output, {"port": port})
    expect(
        "component_mysql_empty_recordset.asp", "True|True|True|1",
        {"port": port, "database": "zblogasp_mysql"},
    )
    local_fixture = ROOT / "tests/fixtures/common/mysql_sliding_window.asp"
    result = subprocess.run(
        [
            str(OPENASP), str(local_fixture), "--root",
            str(local_fixture.parent), "--query",
            urllib.parse.urlencode({"port": port}),
        ],
        capture_output=True, text=True, timeout=25,
        env={**os.environ, "LC_ALL": "C"},
    )
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "200:200:200:True", result.stdout
    print("PASS mysql_sliding_window.asp")
    check_mysql_getrows_column_order(port)
    check_mysql_worker_pool(port)


def check_redis_configuration_properties(port: int) -> None:
    fixture = ROOT / "tests/conformance/cases/redis_configuration_properties.asp"
    result = subprocess.run(
        [
            str(OPENASP), str(fixture), "--root", str(fixture.parent),
            "--query", urllib.parse.urlencode({"port": port}),
        ],
        capture_output=True, text=True, timeout=25,
        env={**os.environ, "LC_ALL": "C"},
    )
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == f"127.0.0.1|{port}|2|3210|0|1|0", result.stdout
    print("PASS redis_configuration_properties.asp")


def check_mysql_worker_pool(port: int) -> None:
    fixture_root = ROOT / "tests/fixtures/common"
    with tempfile.TemporaryDirectory(prefix="openasp-mysql-pool-") as directory:
        socket_path = Path(directory) / "openasp.sock"
        command = [
            str(OPENASP_FPM),
            "--root", str(fixture_root),
            "--unix-socket", str(socket_path),
            "--max-accepts", "2",
            "--workers", "1",
            "--opcache-dir", str(Path(directory) / "cache"),
        ]
        server = start_server(command, socket_path)
        params = {
            "SCRIPT_FILENAME": str(fixture_root / "mysql_pool.asp"),
            "SCRIPT_NAME": "/mysql_pool.asp",
            "REQUEST_METHOD": "GET",
            "QUERY_STRING": urllib.parse.urlencode({"port": port}),
            "SERVER_NAME": "localhost",
            "SERVER_PORT": "80",
        }
        try:
            first = transact(socket_path, params).split(b"\r\n\r\n", 1)[1]
            second = transact(socket_path, params).split(b"\r\n\r\n", 1)[1]
            assert first.strip().isdigit(), first
            assert second.strip() == first.strip(), (first, second)
            assert server.wait(timeout=5) == 0
        finally:
            if server.poll() is None:
                server.terminate()
                server.wait(timeout=5)
    print("PASS MySQL worker connection pool and lazy Recordset")


def check_mysql_getrows_column_order(port: int) -> None:
    fixture = ROOT / "tests/conformance/cases/ado_mysql_getrows_column_order.asp"
    expected = "r1-alpha|r1-beta|r1-gamma|r2-alpha|r2-beta|r2-gamma|True|False"
    for buffered in ("False", "True"):
        result = subprocess.run(
            [
                str(OPENASP), str(fixture), "--root", str(fixture.parent), "--query",
                urllib.parse.urlencode({"port": port, "buffered": buffered}),
            ],
            capture_output=True, text=True, timeout=25,
            env={**os.environ, "LC_ALL": "C"},
        )
        assert result.returncode == 0, result.stderr
        assert result.stdout.strip() == expected, result.stdout
    print("PASS ado_mysql_getrows_column_order.asp (streaming and buffered)")


def check_shell() -> None:
    enabled = Path("/tmp/egret-asp-shell-enabled")
    literal = Path("/tmp/egret-shell-literal;touch egret-shell-injected")
    injected = Path("/tmp/egret-shell-injected")
    for path in (enabled, literal, injected):
        path.unlink(missing_ok=True)
    expect("component_shell_enabled.asp", "0|", None, "--shell-enable=on")
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline and not enabled.exists():
        time.sleep(0.02)
    assert enabled.exists()
    expect(
        "component_shell_errors.asp",
        "launch=5:ShellExecute failed: cannot launch executable: "
        "No such file or directory (errno=2)\n"
        "verb=5:ShellExecute only supports the open operation; received: runas",
        None, "--shell-enable=on",
    )
    expect("component_shell_literal_args.asp", "0|", None,
           "--shell-enable=on")
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline and not literal.exists():
        time.sleep(0.02)
    assert literal.exists() and not injected.exists()
    enabled.unlink(missing_ok=True)
    literal.unlink(missing_ok=True)


def check_process_status_properties() -> None:
    fixture = ROOT / "tests/conformance/cases/process_status_properties.asp"
    result = subprocess.run(
        [
            str(OPENASP), str(fixture), "--root", str(fixture.parent),
            "--process-enable=on",
        ],
        capture_output=True, text=True, timeout=25,
        env={**os.environ, "LC_ALL": "C"},
    )
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "True|True|True|143|True|15", result.stdout
    print("PASS process_status_properties.asp")


def check_mdb() -> None:
    site_path = os.environ.get("OPENASP_QUICKER_SITE_MDB")
    labels_path = os.environ.get("OPENASP_QUICKER_LABELS_MDB")
    assert site_path, "set OPENASP_QUICKER_SITE_MDB to the QuickerSite.mdb path"
    assert labels_path, "set OPENASP_QUICKER_LABELS_MDB to the QuickerLabels.mdb path"
    site = Path(site_path)
    labels = Path(labels_path)
    backlighting = ROOT / "tests/e2e/apps/backlighting-cms/data_act/#BackLightingDatabase.mdb"
    assert site.exists() and labels.exists(), "QuickerSite MDB fixtures missing"
    assert backlighting.exists(), "BacklightingCMS MDB fixture missing"
    expect("ado_mdb_compound_where.asp", "", {"db": site})
    expect("ado_mdb_join.asp", "676:thankyou:Thank you", {"db": labels})
    expect("ado_mdb_top.asp", "default", {"db": backlighting})
    expect("ado_mdb_bracket_clause.asp", "default", {"db": backlighting})
    fixture = ROOT / "tests/conformance/cases/ado_mdb_execute_write.asp"
    with tempfile.TemporaryDirectory() as temporary:
        database = Path(temporary) / "write-test.mdb"
        shutil.copyfile(backlighting, database)
        result = subprocess.run(
            [str(OPENASP), str(fixture), "--root", str(fixture.parent),
             "--query", urllib.parse.urlencode({"db": database})],
            capture_output=True, text=True, timeout=25, env={**os.environ, "LC_ALL": "C"},
        )
        assert result.returncode == 0, result.stderr
        assert result.stdout.strip() == "10.20.30.40", result.stdout
        print("PASS ado_mdb_execute_write.asp")
        fixture = ROOT / "tests/conformance/cases/ado_mdb_recordset_compound_update.asp"
        result = subprocess.run(
            [str(OPENASP), str(fixture), "--root", str(fixture.parent),
             "--query", urllib.parse.urlencode({"db": database})],
            capture_output=True, text=True, timeout=25, env={**os.environ, "LC_ALL": "C"},
        )
        assert result.returncode == 0, result.stderr
        assert result.stdout.strip() == "updated", result.stdout
        print("PASS ado_mdb_recordset_compound_update.asp")
        fixture = ROOT / "tests/conformance/cases/ado_mdb_execute_complex_dml.asp"
        result = subprocess.run(
            [str(OPENASP), str(fixture), "--root", str(fixture.parent),
             "--query", urllib.parse.urlencode({"db": database})],
            capture_output=True, text=True, timeout=25, env={**os.environ, "LC_ALL": "C"},
        )
        assert result.returncode == 0, result.stderr
        assert result.stdout.strip() == "seed-ok:seed", result.stdout
        print("PASS ado_mdb_execute_complex_dml.asp")
        fixture = ROOT / "tests/conformance/cases/ado_mdb_transaction_atomic.asp"
        result = subprocess.run(
            [str(OPENASP), str(fixture), "--root", str(fixture.parent),
             "--query", urllib.parse.urlencode({"db": database})],
            capture_output=True, text=True, timeout=25, env={**os.environ, "LC_ALL": "C"},
        )
        assert result.returncode == 0, result.stderr
        assert result.stdout.strip() == "rolled:two", result.stdout
        print("PASS ado_mdb_transaction_atomic.asp")
        fixture = ROOT / "tests/conformance/cases/ado_mdb_group_by.asp"
        result = subprocess.run(
            [str(OPENASP), str(fixture), "--root", str(fixture.parent),
             "--query", urllib.parse.urlencode({"db": database})],
            capture_output=True, text=True, timeout=25, env={**os.environ, "LC_ALL": "C"},
        )
        assert result.returncode == 0, result.stderr
        assert result.stdout.strip() == "a:2:7", result.stdout
        print("PASS ado_mdb_group_by.asp")
        fixture = ROOT / "tests/conformance/cases/ado_mdb_union.asp"
        result = subprocess.run(
            [str(OPENASP), str(fixture), "--root", str(fixture.parent),
             "--query", urllib.parse.urlencode({"db": database})],
            capture_output=True, text=True, timeout=25, env={**os.environ, "LC_ALL": "C"},
        )
        assert result.returncode == 0, result.stderr
        assert result.stdout.strip() == "1:True:1:1", result.stdout
        print("PASS ado_mdb_union.asp")
        fixture = ROOT / "tests/conformance/cases/ado_mdb_subquery.asp"
        result = subprocess.run(
            [str(OPENASP), str(fixture), "--root", str(fixture.parent),
             "--query", urllib.parse.urlencode({"db": database})],
            capture_output=True, text=True, timeout=25, env={**os.environ, "LC_ALL": "C"},
        )
        assert result.returncode == 0, result.stderr
        assert result.stdout.strip() == "2", result.stdout
        print("PASS ado_mdb_subquery.asp")
        fixture = ROOT / "tests/conformance/cases/ado_mdb_correlated_derived.asp"
        result = subprocess.run(
            [str(OPENASP), str(fixture), "--root", str(fixture.parent),
             "--query", urllib.parse.urlencode({"db": database})],
            capture_output=True, text=True, timeout=25, env={**os.environ, "LC_ALL": "C"},
        )
        assert result.returncode == 0, result.stderr
        assert result.stdout.strip() == "2:2", result.stdout
        print("PASS ado_mdb_correlated_derived.asp")
        fixture = ROOT / "tests/conformance/cases/ado_mdb_common_select.asp"
        result = subprocess.run(
            [str(OPENASP), str(fixture), "--root", str(fixture.parent),
             "--query", urllib.parse.urlencode({"db": database})],
            capture_output=True, text=True, timeout=25, env={**os.environ, "LC_ALL": "C"},
        )
        assert result.returncode == 0, result.stderr
        assert result.stdout.strip() == "alpha:beta:3:3:0:1:ALPHA:5:al:lp:none:1:one:2:True:2:1", result.stdout
        print("PASS ado_mdb_common_select.asp")
        fixture = ROOT / "tests/conformance/cases/ado_mdb_nested_join.asp"
        nested_database = Path(temporary) / "nested-join-test.mdb"
        shutil.copyfile(backlighting, nested_database)
        result = subprocess.run(
            [str(OPENASP), str(fixture), "--root", str(fixture.parent),
             "--query", urllib.parse.urlencode({"db": nested_database})],
            capture_output=True, text=True, timeout=25, env={**os.environ, "LC_ALL": "C"},
        )
        assert result.returncode == 0, result.stderr
        assert result.stdout.strip() == "motor", result.stdout
        print("PASS ado_mdb_nested_join.asp")
        fixture = ROOT / "tests/conformance/cases/ado_mdb_memo_long_value.asp"
        result = subprocess.run(
            [str(OPENASP), str(fixture), "--root", str(fixture.parent),
             "--query", urllib.parse.urlencode({"db": database})],
            capture_output=True, text=True, timeout=25, env={**os.environ, "LC_ALL": "C"},
        )
        assert result.returncode == 0, result.stderr
        assert result.stdout.strip() == "3000", result.stdout
        print("PASS ado_mdb_memo_long_value.asp")
        fixture = ROOT / "tests/conformance/cases/ado_mdb_memo_update.asp"
        result = subprocess.run(
            [str(OPENASP), str(fixture), "--root", str(fixture.parent),
             "--query", urllib.parse.urlencode({"db": database})],
            capture_output=True, text=True, timeout=25, env={**os.environ, "LC_ALL": "C"},
        )
        assert result.returncode == 0, result.stderr
        assert result.stdout.strip() == "3001", result.stdout
        print("PASS ado_mdb_memo_update.asp")
        fixture = ROOT / "tests/conformance/cases/ado_mdb_index_ddl.asp"
        result = subprocess.run(
            [str(OPENASP), str(fixture), "--root", str(fixture.parent),
             "--query", urllib.parse.urlencode({"db": database})],
            capture_output=True, text=True, timeout=25, env={**os.environ, "LC_ALL": "C"},
        )
        assert result.returncode == 0, result.stderr
        assert result.stdout.strip() == "True:True:6", result.stdout
        print("PASS ado_mdb_index_ddl.asp")
    with tempfile.TemporaryDirectory() as temporary:
        database = Path(temporary) / "ddl-test.mdb"
        shutil.copyfile(backlighting, database)
        fixture = ROOT / "tests/conformance/cases/ado_mdb_execute_ddl.asp"
        result = subprocess.run(
            [str(OPENASP), str(fixture), "--root", str(fixture.parent),
             "--query", urllib.parse.urlencode({"db": database})],
            capture_output=True, text=True, timeout=25, env={**os.environ, "LC_ALL": "C"},
        )
        assert result.returncode == 0, result.stderr
        assert result.stdout.strip() == "created:42:True:7", result.stdout
        print("PASS ado_mdb_execute_ddl.asp")


def main() -> None:
    try:
        assert OPENASP.exists(), f"build first: {OPENASP}"
        assert FIXTURES.exists(), f"reference fixtures missing: {FIXTURES}"
        check_reference_backend_harness()
        check_http()
        check_http_multi()
        check_redis_multi()
        check_memcache_multi()
        check_memcache_multi_limits()
        check_http_status_properties()
        check_http_tls_configuration_properties()
        check_memcache_configuration_properties()
        check_socket_client()
        check_socket_listener()
        check_socket_udp()
        check_redis_and_mysql()
        check_shell()
        check_process_status_properties()
        check_mdb()
        print("PASS: all 25 external fixtures")
    finally:
        if OWNED_REDIS is not None and OWNED_REDIS.poll() is None:
            OWNED_REDIS.terminate()
            try:
                OWNED_REDIS.wait(timeout=5)
            except subprocess.TimeoutExpired:
                OWNED_REDIS.kill()
                OWNED_REDIS.wait(timeout=2)


if __name__ == "__main__":
    main()
