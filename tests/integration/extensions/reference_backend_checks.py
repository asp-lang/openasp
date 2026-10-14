# Copyright (c) 2026 OpenASP.dev
# SPDX-License-Identifier: MIT

"""Integration checks for OpenASP backend service components."""

from __future__ import annotations

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import base64
import hashlib
import json
import socket
import socketserver
import ssl
import subprocess
import tempfile
import threading
import time


ROOT = next(parent for parent in Path(__file__).resolve().parents if (parent / "Makefile").is_file())
OPENASP = ROOT / "build/bin/openasp"
FIXTURES = ROOT / "tests/fixtures/reference"


class MockHandler(BaseHTTPRequestHandler):
    """Deterministic HTTP/OpenAI-compatible endpoint for component requests."""

    def do_GET(self) -> None:
        if self.path != "/secure":
            self.send_error(404)
            return
        response = b"secure-ok"
        self.send_response(200)
        self.send_header("Content-Length", str(len(response)))
        self.end_headers()
        self.wfile.write(response)

    def do_POST(self) -> None:
        length = int(self.headers.get("Content-Length", "0"))
        request_body = self.rfile.read(length)
        if self.path == "/echo":
            response = request_body
        elif self.path == "/v1/chat/completions":
            request = json.loads(request_body)
            assert self.headers["Authorization"] == "Bearer test-secret"
            assert request["model"] == "mock-model"
            response = b'{"id":"chat-mock","choices":[{"message":{"content":"ok"}}]}'
        elif self.path == "/v1/responses":
            request = json.loads(request_body)
            assert request["input"] == "raw"
            response = b'{"id":"response-mock","output_text":"ok"}'
        else:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("X-Mock", "yes")
        self.send_header("Content-Length", str(len(response)))
        self.end_headers()
        self.wfile.write(response)

    def log_message(self, format: str, *args: object) -> None:
        pass


def recv_exact(sock: socket.socket, size: int) -> bytes:
    """Receive exactly size bytes or fail when the peer closes early."""
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise EOFError("socket closed")
        data.extend(chunk)
    return bytes(data)


def recv_websocket_frame(sock: socket.socket) -> tuple[int, bytes]:
    """Decode one small RFC 6455 frame used by the local test peer."""
    first, second = recv_exact(sock, 2)
    length = second & 0x7F
    if length == 126:
        length = int.from_bytes(recv_exact(sock, 2), "big")
    elif length == 127:
        length = int.from_bytes(recv_exact(sock, 8), "big")
    mask = recv_exact(sock, 4) if second & 0x80 else b""
    payload = recv_exact(sock, length)
    if mask:
        payload = bytes(value ^ mask[index % 4] for index, value in enumerate(payload))
    return first & 0x0F, payload


def send_websocket_frame(sock: socket.socket, opcode: int, payload: bytes) -> None:
    """Send one unmasked server frame, including the 16-bit length form."""
    if len(payload) <= 125:
        header = bytes((0x80 | opcode, len(payload)))
    else:
        header = bytes((0x80 | opcode, 126)) + len(payload).to_bytes(2, "big")
    sock.sendall(header + payload)


def websocket_handshake(sock: socket.socket, expected_protocol: str) -> None:
    """Validate the client's subprotocol and complete a server-side upgrade."""
    request = bytearray()
    while b"\r\n\r\n" not in request:
        request.extend(sock.recv(4096))
    headers = {}
    for line in bytes(request).decode("latin1").split("\r\n")[1:]:
        if ":" in line:
            key, value = line.split(":", 1)
            headers[key.lower()] = value.strip()
    assert headers["sec-websocket-protocol"] == expected_protocol
    accept = base64.b64encode(
        hashlib.sha1((headers["sec-websocket-key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()
    ).decode()
    sock.sendall(
        (
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Accept: {accept}\r\n"
            f"Sec-WebSocket-Protocol: {expected_protocol}\r\n\r\n"
        ).encode()
    )


def websocket_client_mock(port: int = 0, tls_context: ssl.SSLContext | None = None,
                          ready: threading.Event | None = None,
                          bound_port: list[int] | None = None) -> None:
    """Run one WebSocket peer and assert text, binary, ping, and pong behavior."""
    with socket.create_server(("127.0.0.1", port)) as listener:
        listener.settimeout(10)
        if bound_port is not None:
            bound_port.append(listener.getsockname()[1])
        if ready is not None:
            ready.set()
        conn, _ = listener.accept()
        with conn:
            if tls_context is not None:
                conn = tls_context.wrap_socket(conn, server_side=True)
            conn.settimeout(5)
            websocket_handshake(conn, "openasp.test")
            assert recv_websocket_frame(conn) == (1, b"hello")
            assert recv_websocket_frame(conn) == (2, b"\x00\xff")
            send_websocket_frame(conn, 9, b"p")
            send_websocket_frame(conn, 1, b"world")
            assert recv_websocket_frame(conn) == (10, b"p")


class MemcacheHandler(socketserver.StreamRequestHandler):
    """Minimal text-protocol server with shared values and monotonic CAS ids."""

    values: dict[str, tuple[bytes, int, int]] = {}
    next_cas = 40

    def handle(self) -> None:
        while line := self.rfile.readline():
            parts = line.rstrip(b"\r\n").split()
            command = parts[0].lower()
            if command in {b"set", b"add", b"replace", b"append", b"prepend", b"cas"}:
                key = parts[1].decode()
                flags = int(parts[2])
                size = int(parts[4])
                value = self.rfile.read(size)
                assert self.rfile.read(2) == b"\r\n"
                current = self.values.get(key)
                if command == b"add" and current is not None:
                    self.wfile.write(b"NOT_STORED\r\n")
                    continue
                if command == b"replace" and current is None:
                    self.wfile.write(b"NOT_STORED\r\n")
                    continue
                if command == b"append" and current is not None:
                    value = current[0] + value
                    flags = current[1]
                if command == b"prepend" and current is not None:
                    value = value + current[0]
                    flags = current[1]
                self.__class__.next_cas += 1
                self.values[key] = (value, flags, self.next_cas)
                self.wfile.write(b"STORED\r\n")
            elif command in {b"get", b"gets"}:
                for raw_key in parts[1:]:
                    key = raw_key.decode()
                    if key in self.values:
                        value, flags, cas = self.values[key]
                        suffix = f" {cas}" if command == b"gets" else ""
                        self.wfile.write(f"VALUE {key} {flags} {len(value)}{suffix}\r\n".encode() + value + b"\r\n")
                self.wfile.write(b"END\r\n")
            elif command in {b"incr", b"decr"}:
                key = parts[1].decode()
                delta = int(parts[2])
                value, flags, _ = self.values[key]
                number = int(value)
                number = number + delta if command == b"incr" else max(0, number - delta)
                self.__class__.next_cas += 1
                self.values[key] = (str(number).encode(), flags, self.next_cas)
                self.wfile.write(f"{number}\r\n".encode())
            elif command == b"delete":
                existed = self.values.pop(parts[1].decode(), None) is not None
                self.wfile.write(b"DELETED\r\n" if existed else b"NOT_FOUND\r\n")
            elif command == b"touch":
                self.wfile.write(b"TOUCHED\r\n" if parts[1].decode() in self.values else b"NOT_FOUND\r\n")
            elif command == b"version":
                self.wfile.write(b"VERSION mock-1.0\r\n")
            elif command == b"stats":
                self.wfile.write(f"STAT curr_items {len(self.values)}\r\nEND\r\n".encode())
            elif command == b"flush_all":
                self.values.clear()
                self.wfile.write(b"OK\r\n")
            else:
                self.wfile.write(b"ERROR\r\n")


class MemcacheServer(socketserver.ThreadingTCPServer):
    """Reusable local endpoint so repeated runs do not wait for TIME_WAIT."""

    allow_reuse_address = True


def run_fixture(name: str, *extra: str) -> subprocess.CompletedProcess[str]:
    """Render one ASP fixture against the freshly built standalone binary."""
    return subprocess.run(
        [
            str(OPENASP),
            str(FIXTURES / name),
            "--root",
            str(FIXTURES),
            "--log-file",
            "off",
            *extra,
        ],
        capture_output=True,
        text=True,
        timeout=20,
    )


def unused_tcp_port() -> int:
    """Reserve an ephemeral port number for the short-lived ASP listener."""
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def check_websocket_server(port: int) -> None:
    """Drive the ASP server-side upgrade path with a masked client frame."""
    process = subprocess.Popen(
        [
            str(OPENASP),
            str(FIXTURES / "component_websocket_server.asp"),
            "--root",
            str(FIXTURES),
            "--log-file",
            "off",
            "--query",
            f"port={port}",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    deadline = time.monotonic() + 10
    conn = None
    while time.monotonic() < deadline:
        try:
            conn = socket.create_connection(("127.0.0.1", port), timeout=1)
            break
        except OSError:
            time.sleep(0.05)
    assert conn is not None, "WebSocket ASP server did not start"
    with conn:
        key = base64.b64encode(b"0123456789abcdef").decode()
        conn.sendall(
            (
                "GET /server HTTP/1.1\r\n"
                f"Host: 127.0.0.1:{port}\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                f"Sec-WebSocket-Key: {key}\r\n"
                "Sec-WebSocket-Version: 13\r\n\r\n"
            ).encode()
        )
        response = bytearray()
        while b"\r\n\r\n" not in response:
            response.extend(conn.recv(4096))
        assert b"101 Switching Protocols" in response
        payload = b"server-test"
        mask = b"\x01\x02\x03\x04"
        masked = bytes(value ^ mask[index % 4] for index, value in enumerate(payload))
        conn.sendall(bytes((0x81, 0x80 | len(payload))) + mask + masked)
        assert recv_websocket_frame(conn) == (1, b"reply:server-test")
    stdout, stderr = process.communicate(timeout=10)
    assert process.returncode == 0, stderr
    assert stdout.strip() == "1|text|0", stdout


def main() -> None:
    """Start protocol mocks, execute component fixtures, and clean up all threads."""
    server = ThreadingHTTPServer(("127.0.0.1", 0), MockHandler)
    memcache = MemcacheServer(("127.0.0.1", 0), MemcacheHandler)
    websocket_server_port = unused_tcp_port()
    websocket_ready = threading.Event()
    websocket_ports: list[int] = []
    websocket_thread = threading.Thread(
        target=websocket_client_mock,
        args=(0, None, websocket_ready, websocket_ports),
        daemon=True,
    )
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    memcache_thread = threading.Thread(target=memcache.serve_forever, daemon=True)
    thread.start()
    memcache_thread.start()
    websocket_thread.start()
    assert websocket_ready.wait(timeout=5)
    assert websocket_ports
    try:
        result = run_fixture("component_backend_services.asp", "--query", f"port={server.server_port}")
        assert result.returncode == 0, result.stderr
        output = result.stdout
        assert output.startswith('OpenASP|1|True|{"name":"OpenASP"'), output
        assert '|True|{"x":1}|False|4|12|' in output, output
        assert "ba7816bf8f01cfea414140de5dae2223" in output, output
        assert "f7bc83f430538424b13298e6aa6fb143" in output, output
        assert "|T3BlbkFTUA==|OpenASP|AP8=|True|16|200|" in output, output
        assert '{"hello":"world"}|yes|200|' in output, output
        assert '"id":"chat-mock"' in output and '"id":"response-mock"' in output, output
        conformance = ROOT / "tests/conformance/cases/openai_status_properties.asp"
        openai_status = subprocess.run(
            [
                str(OPENASP), str(conformance), "--root", str(conformance.parent),
                "--query", f"port={server.server_port}",
            ],
            capture_output=True, text=True, timeout=20,
        )
        assert openai_status.returncode == 0, openai_status.stderr
        expected_response = '{"id":"chat-mock","choices":[{"message":{"content":"ok"}}]}'
        assert openai_status.stdout.strip() == f"200|{expected_response}|{expected_response}", openai_status.stdout

        denied = run_fixture("component_process.asp")
        assert denied.returncode != 0

        process = run_fixture("component_process.asp", "--process-enable=on")
        assert process.returncode == 0, process.stderr
        assert process.stdout.strip() == "7|stdout|stderr|False|-1|True|True|True|True|143|3|True|True|23|0|True|0|True|True|-1|15", process.stdout

        websocket_client = run_fixture("component_websocket_client.asp", "--query", f"port={websocket_ports[0]}")
        assert websocket_client.returncode == 0, websocket_client.stderr
        assert websocket_client.stdout.strip() == "1|True|True|world|text|1|True|0", websocket_client.stdout
        websocket_thread.join(timeout=5)
        assert not websocket_thread.is_alive()

        websocket_ready = threading.Event()
        websocket_ports = []
        websocket_thread = threading.Thread(
            target=websocket_client_mock,
            args=(0, None, websocket_ready, websocket_ports),
            daemon=True,
        )
        websocket_thread.start()
        assert websocket_ready.wait(timeout=5)
        assert websocket_ports
        conformance = ROOT / "tests/conformance/cases/websocket_status_properties.asp"
        websocket_properties = subprocess.run(
            [
                str(OPENASP), str(conformance), "--root", str(conformance.parent),
                "--query", f"port={websocket_ports[0]}",
            ],
            capture_output=True, text=True, timeout=20,
        )
        assert websocket_properties.returncode == 0, websocket_properties.stderr
        assert websocket_properties.stdout.strip() == "1|True|True|world|text|1|True|0|0", websocket_properties.stdout
        websocket_thread.join(timeout=5)
        assert not websocket_thread.is_alive()

        check_websocket_server(websocket_server_port)

        cache = run_fixture("component_memcache.asp", "--query", f"port={memcache.server_address[1]}")
        assert cache.returncode == 0, cache.stderr
        assert cache.stdout.strip() == (
            f"127.0.0.1|{memcache.server_address[1]}|text|True|1|True|hello|7|hello|41|"
            "True|True|>hello!|True|True|True|8|6|True|mock-1.0|3|True|True|True|0"
        ), cache.stdout

        with tempfile.TemporaryDirectory() as directory:
            cert = Path(directory) / "cert.pem"
            key = Path(directory) / "key.pem"
            subprocess.run(
                [
                    "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                    "-keyout", str(key), "-out", str(cert), "-days", "1",
                    "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost",
                ],
                check=True,
                capture_output=True,
            )
            tls_server = ThreadingHTTPServer(("127.0.0.1", 0), MockHandler)
            tls_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            tls_context.load_cert_chain(cert, key)
            tls_server.socket = tls_context.wrap_socket(tls_server.socket, server_side=True)
            tls_thread = threading.Thread(target=tls_server.serve_forever, daemon=True)
            tls_thread.start()
            try:
                rejected = run_fixture("component_https.asp", "--query", f"port={tls_server.server_port}")
                assert rejected.returncode == 0, rejected.stderr
                assert "|12004|HTTP request failed" in rejected.stdout, rejected.stdout
                verified = run_fixture(
                    "component_https.asp",
                    "--query",
                    f"ca={cert}&port={tls_server.server_port}",
                )
                assert verified.returncode == 0, verified.stderr
                assert verified.stdout.strip() == "200|secure-ok|True|0|", verified.stdout

                wss_ready = threading.Event()
                wss_ports: list[int] = []
                wss_thread = threading.Thread(
                    target=websocket_client_mock,
                    args=(0, tls_context, wss_ready, wss_ports),
                    daemon=True,
                )
                wss_thread.start()
                assert wss_ready.wait(timeout=5)
                assert wss_ports
                wss = run_fixture(
                    "component_websocket_wss.asp",
                    "--query",
                    f"ca={cert}&port={wss_ports[0]}",
                )
                assert wss.returncode == 0, wss.stderr
                assert wss.stdout.strip() == "1|True|True|world|text|1|True|0", wss.stdout
                wss_thread.join(timeout=5)
                assert not wss_thread.is_alive()
            finally:
                tls_server.shutdown()
                tls_server.server_close()
                tls_thread.join(timeout=2)
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)
        memcache.shutdown()
        memcache.server_close()
        memcache_thread.join(timeout=2)

    print("PASS: OpenASP backend components, HTTPS, WebSocket, and Memcache")


if __name__ == "__main__":
    main()
