#!/usr/bin/env python3
# Copyright (c) 2026 https://asp-lang.org
# SPDX-License-Identifier: MIT

"""Isolated Redis and Memcache text-protocol services for blog benchmarks."""

from __future__ import annotations

from pathlib import Path
import shutil
import socket
import socketserver
import subprocess
import tempfile
import threading
import time


ROOT = next(parent for parent in Path(__file__).resolve().parents if (parent / "Makefile").is_file())


class MemcacheHandler(socketserver.StreamRequestHandler):
    values: dict[str, tuple[bytes, int]] = {}
    lock = threading.Lock()

    def handle(self) -> None:
        while line := self.rfile.readline():
            parts = line.rstrip(b"\r\n").split()
            if not parts:
                continue
            command = parts[0].lower()
            if command == b"set" and len(parts) >= 5:
                key = parts[1].decode("ascii")
                flags = int(parts[2])
                size = int(parts[4])
                value = self.rfile.read(size)
                if self.rfile.read(2) != b"\r\n":
                    return
                with self.lock:
                    self.values[key] = (value, flags)
                self.wfile.write(b"STORED\r\n")
            elif command == b"get":
                with self.lock:
                    found = [(key, self.values.get(key)) for key in map(bytes.decode, parts[1:])]
                for key, item in found:
                    if item is not None:
                        value, flags = item
                        self.wfile.write(
                            f"VALUE {key} {flags} {len(value)}\r\n".encode("ascii") + value + b"\r\n"
                        )
                self.wfile.write(b"END\r\n")
            elif command == b"flush_all":
                with self.lock:
                    self.values.clear()
                self.wfile.write(b"OK\r\n")
            elif command == b"version":
                self.wfile.write(b"VERSION benchmark-1.0\r\n")
            else:
                self.wfile.write(b"ERROR\r\n")


class MemcacheServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True
    request_queue_size = 128


class BackendServices:
    def __init__(self, redis_port: int = 16379, memcache_port: int = 19146) -> None:
        self.redis_port = redis_port
        self.memcache_port = memcache_port
        self.redis: subprocess.Popen[bytes] | None = None
        self.memcached: subprocess.Popen[bytes] | None = None
        self.memcache: MemcacheServer | None = None
        self.memcache_thread: threading.Thread | None = None
        self.temporary: tempfile.TemporaryDirectory[str] | None = None

    def __enter__(self) -> "BackendServices":
        redis_server = shutil.which("redis-server")
        if redis_server is None:
            raise RuntimeError("redis-server is required")
        try:
            self.temporary = tempfile.TemporaryDirectory(prefix="zblog-redis-")
            directory = Path(self.temporary.name)
            self.redis = subprocess.Popen(
                [
                    redis_server,
                    "--bind", "127.0.0.1",
                    "--port", str(self.redis_port),
                    "--save", "",
                    "--appendonly", "no",
                    "--dir", str(directory),
                    "--daemonize", "no",
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            memcached = shutil.which("memcached")
            built_memcached = ROOT / "build" / "tools" / "memcached-1.6.45" / "memcached"
            if memcached is None and built_memcached.is_file():
                memcached = str(built_memcached)
            if memcached is not None:
                self.memcached = subprocess.Popen(
                    [
                        memcached,
                        "-l", "127.0.0.1",
                        "-p", str(self.memcache_port),
                        "-U", "0",
                        "-m", "16",
                        "-c", "1024",
                        "-t", "4",
                    ],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )
            else:
                self.memcache = MemcacheServer(("127.0.0.1", self.memcache_port), MemcacheHandler)
                self.memcache_thread = threading.Thread(target=self.memcache.serve_forever, daemon=True)
                self.memcache_thread.start()
            self._wait_for_redis()
            self._wait_for_memcache()
            return self
        except BaseException:
            self.__exit__(None, None, None)
            raise

    def _wait_for_redis(self) -> None:
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if self.redis is not None and self.redis.poll() is not None:
                stderr = self.redis.stderr.read().decode(errors="replace") if self.redis.stderr else ""
                raise RuntimeError(f"redis-server exited during startup: {stderr}")
            try:
                with socket.create_connection(("127.0.0.1", self.redis_port), timeout=0.1) as client:
                    client.sendall(b"*1\r\n$4\r\nPING\r\n")
                    if client.recv(64).startswith(b"+PONG"):
                        return
            except OSError:
                time.sleep(0.02)
        raise RuntimeError("redis-server did not become ready")

    def _wait_for_memcache(self) -> None:
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if self.memcached is not None and self.memcached.poll() is not None:
                stderr = self.memcached.stderr.read().decode(errors="replace") if self.memcached.stderr else ""
                raise RuntimeError(f"memcached exited during startup: {stderr}")
            try:
                with socket.create_connection(("127.0.0.1", self.memcache_port), timeout=0.1) as client:
                    client.sendall(b"version\r\n")
                    if client.recv(128).startswith(b"VERSION"):
                        return
            except OSError:
                time.sleep(0.02)
        raise RuntimeError("memcached did not become ready")

    def __exit__(self, exc_type: object, exc: object, traceback: object) -> None:
        if self.memcache is not None:
            self.memcache.shutdown()
            self.memcache.server_close()
        if self.memcache_thread is not None:
            self.memcache_thread.join(timeout=2)
        if self.memcached is not None:
            self.memcached.terminate()
            try:
                self.memcached.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.memcached.kill()
                self.memcached.wait(timeout=2)
        if self.redis is not None:
            self.redis.terminate()
            try:
                self.redis.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.redis.kill()
                self.redis.wait(timeout=2)
        if self.temporary is not None:
            self.temporary.cleanup()
