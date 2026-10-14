#!/usr/bin/env python3
# Copyright (c) 2026 https://asp-lang.org
# SPDX-License-Identifier: MIT

"""Exercise worker-local Redis/Memcache reuse and Memcache multi-get."""

from __future__ import annotations

import os
from pathlib import Path
import socketserver
import sys
import tempfile
import threading
import urllib.parse

sys.path.insert(0, str(next(parent for parent in Path(__file__).resolve().parents if (parent / "Makefile").is_file()) / "tests/integration/fastcgi"))
from test_fastcgi import start_server, transact


ROOT = next(parent for parent in Path(__file__).resolve().parents if (parent / "Makefile").is_file())
OPENASP_FPM = Path(os.environ.get("OPENASP_FPM", ROOT / "build" / "bin" / "openasp-fpm"))
FIXTURE_ROOT = ROOT / "tests" / "fixtures" / "common"


class CountingServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, handler: type[socketserver.StreamRequestHandler]) -> None:
        super().__init__(("127.0.0.1", 0), handler)
        self.connections = 0
        self.commands = 0
        self.lock = threading.Lock()

    def get_request(self) -> tuple[object, object]:
        request = super().get_request()
        with self.lock:
            self.connections += 1
        return request


class RedisHandler(socketserver.StreamRequestHandler):
    def handle(self) -> None:
        while marker := self.rfile.read(1):
            if marker != b"*":
                return
            count = int(self.rfile.readline().rstrip(b"\r\n"))
            arguments = []
            for _ in range(count):
                if self.rfile.read(1) != b"$":
                    return
                length = int(self.rfile.readline().rstrip(b"\r\n"))
                arguments.append(self.rfile.read(length))
                if self.rfile.read(2) != b"\r\n":
                    return
            if arguments and arguments[0].upper() == b"PING":
                self.wfile.write(b"+PONG\r\n")
            elif arguments and arguments[0].upper() in (b"SELECT", b"MULTI"):
                self.wfile.write(b"+OK\r\n")
            else:
                self.wfile.write(b"-ERR unsupported\r\n")


class RedisDropHandler(RedisHandler):
    def handle(self) -> None:
        server = self.server
        assert isinstance(server, CountingServer)
        with server.lock:
            first = server.commands == 0
            server.commands += 1
        if first:
            self.rfile.readline()
            self.wfile.write(b"+PO")
            return
        super().handle()


class MemcacheHandler(socketserver.StreamRequestHandler):
    values: dict[str, bytes] = {}
    lock = threading.Lock()

    def handle(self) -> None:
        while line := self.rfile.readline():
            parts = line.rstrip(b"\r\n").split()
            if not parts:
                continue
            command = parts[0].lower()
            if command == b"set" and len(parts) >= 5:
                key = parts[1].decode("ascii")
                length = int(parts[4])
                value = self.rfile.read(length)
                if self.rfile.read(2) != b"\r\n":
                    return
                with self.lock:
                    self.values[key] = value
                self.wfile.write(b"STORED\r\n")
            elif command == b"get":
                with self.lock:
                    values = [(key.decode("ascii"), self.values.get(key.decode("ascii"))) for key in parts[1:]]
                for key, value in values:
                    if value is not None:
                        self.wfile.write(f"VALUE {key} 0 {len(value)}\r\n".encode("ascii"))
                        self.wfile.write(value + b"\r\n")
                self.wfile.write(b"END\r\n")
            else:
                self.wfile.write(b"ERROR\r\n")


def serve(server: CountingServer) -> threading.Thread:
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return thread


def run_requests(fixture: str, query: dict[str, int], expected: list[bytes]) -> None:
    with tempfile.TemporaryDirectory(prefix="openasp-kv-pool-") as directory:
        socket_path = Path(directory) / "openasp.sock"
        command = [
            str(OPENASP_FPM),
            "--root", str(FIXTURE_ROOT),
            "--unix-socket", str(socket_path),
            "--max-accepts", str(len(expected)),
            "--workers", "1",
            "--opcache-dir", str(Path(directory) / "cache"),
        ]
        process = start_server(command, socket_path)
        params = {
            "SCRIPT_FILENAME": str(FIXTURE_ROOT / fixture),
            "SCRIPT_NAME": f"/{fixture}",
            "REQUEST_METHOD": "GET",
            "QUERY_STRING": urllib.parse.urlencode(query),
            "SERVER_NAME": "localhost",
            "SERVER_PORT": "80",
        }
        try:
            for expected_body in expected:
                response = transact(socket_path, params)
                body = response.split(b"\r\n\r\n", 1)[1]
                assert body.strip() == expected_body, body
            assert process.wait(timeout=5) == 0
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=5)


def main() -> None:
    redis = CountingServer(RedisHandler)
    memcache = CountingServer(MemcacheHandler)
    threads = [serve(redis), serve(memcache)]
    try:
        run_requests(
            "kv_pool.asp",
            {"redis_port": redis.server_address[1], "memcache_port": memcache.server_address[1]},
            [b"PONG|2|alpha|beta|False"] * 3,
        )
        assert redis.connections == 1, redis.connections
        assert memcache.connections == 1, memcache.connections
        print("PASS Redis/Memcache worker pool and Memcache GetMulti")
    finally:
        redis.shutdown()
        memcache.shutdown()
        redis.server_close()
        memcache.server_close()
        for thread in threads:
            thread.join(timeout=2)

    dirty = CountingServer(RedisHandler)
    thread = serve(dirty)
    try:
        run_requests("redis_dirty_pool.asp", {"redis_port": dirty.server_address[1]}, [b"OK", b"OK"])
        assert dirty.connections == 2, dirty.connections
        print("PASS Redis transaction state rejects pooling")
    finally:
        dirty.shutdown()
        dirty.server_close()
        thread.join(timeout=2)

    dropped = CountingServer(RedisDropHandler)
    thread = serve(dropped)
    try:
        run_requests("redis_reconnect.asp", {"redis_port": dropped.server_address[1]}, [b"error", b"PONG"])
        assert dropped.connections == 2, dropped.connections
        print("PASS Redis partial response eviction and reconnect")
    finally:
        dropped.shutdown()
        dropped.server_close()
        thread.join(timeout=2)


if __name__ == "__main__":
    main()
