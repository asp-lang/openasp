#!/usr/bin/env python3
# Copyright (c) 2026 https://asp-lang.org
# SPDX-License-Identifier: MIT

"""Verify openasp-fpm YAML loading, command-line precedence, and daemon mode."""

from __future__ import annotations

from pathlib import Path
import datetime
import socket
import struct
import subprocess
import sys
import tempfile
import time


def record(kind: int, request_id: int, content: bytes) -> bytes:
    return struct.pack("!BBHHBB", 1, kind, request_id, len(content), 0, 0) + content


def parameter(name: str, value: str) -> bytes:
    name_bytes = name.encode()
    value_bytes = value.encode()
    return bytes([len(name_bytes), len(value_bytes)]) + name_bytes + value_bytes


def transact(socket_path: Path, script: Path) -> bytes:
    params = {
        "SCRIPT_FILENAME": str(script),
        "SCRIPT_NAME": f"/{script.name}",
        "REQUEST_METHOD": "GET",
        "SERVER_NAME": "localhost",
        "SERVER_PORT": "80",
    }
    encoded = b"".join(parameter(name, value) for name, value in params.items())
    payload = (
        record(1, 1, struct.pack("!HB5x", 1, 0))
        + record(4, 1, encoded)
        + record(4, 1, b"")
        + record(5, 1, b"")
    )
    output = bytearray()
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(5)
        client.connect(str(socket_path))
        client.sendall(payload)
        while True:
            header = client.recv(8)
            if len(header) != 8:
                raise RuntimeError("unexpected FastCGI response header")
            _, kind, _, length, padding, _ = struct.unpack("!BBHHBB", header)
            content = bytearray()
            while len(content) < length:
                content.extend(client.recv(length - len(content)))
            if padding:
                client.recv(padding)
            if kind == 6:
                output.extend(content)
            if kind == 3:
                return bytes(output)


def wait_for_socket(socket_path: Path, process: subprocess.Popen | None = None) -> None:
    for _ in range(200):
        if socket_path.exists():
            return
        if process is not None and process.poll() is not None:
            stderr = process.stderr.read().decode() if process.stderr else ""
            raise RuntimeError(f"openasp-fpm exited before listening: {stderr}")
        time.sleep(0.01)
    raise RuntimeError(f"socket was not created: {socket_path}")


def write_config(path: Path, values: dict[str, str]) -> None:
    path.write_text("".join(f"{key}: {value}\n" for key, value in values.items()))


def run_foreground(binary: Path, config: Path, socket_path: Path, script: Path,
                   extra: list[str] | None = None) -> bytes:
    process = subprocess.Popen(
        [str(binary), "-c", str(config), *(extra or [])],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    try:
        wait_for_socket(socket_path, process)
        output = transact(socket_path, script)
        assert process.wait(timeout=5) == 0
        return output
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=5)


def main() -> int:
    binary = Path(sys.argv[1]).resolve()
    root = next(parent for parent in Path(__file__).resolve().parents if (parent / "Makefile").is_file()) / "tests/fixtures/common"
    script = root / "minimal_profile.asp"
    with tempfile.TemporaryDirectory(prefix="openasp-fpm-config-") as directory:
        temporary = Path(directory)
        configured_socket = temporary / "configured.sock"
        override_socket = temporary / "override.sock"
        log_base = temporary / "openasp-fpm.log"
        config = temporary / "openasp-fpm.yaml"
        write_config(
            config,
            {
                "root": f"'{root}'",
                "unix-socket": configured_socket,
                "max-accepts": "1",
                "workers": "1",
                "memory-limit": "128M",
                "opcache-dir": f"{temporary / 'cache'} # local cache",
                "gen-dbg-info": "on",
                "log-file": log_base,
                "log-level": "notice",
                "daemon": "off",
            },
        )
        output = run_foreground(binary, config, configured_socket, script)
        assert b"\r\n\r\n42" in output
        log_path = Path(f"{log_base}-{datetime.datetime.now().strftime('%Y%m%d')}")
        log_text = log_path.read_text()
        assert "[notice]" in log_text
        assert "server starting in foreground mode" in log_text
        assert "fastcgi listening" not in log_text

        output = run_foreground(
            binary,
            config,
            override_socket,
            script,
            [
                "--unix-socket", str(override_socket),
                "--opcache-dir", str(temporary / "override-cache"),
                "--gen-dbg-info=off",
            ],
        )
        assert b"\r\n\r\n42" in output
        assert not configured_socket.exists()

        empty = temporary / "empty.yaml"
        empty.write_text("# all values may be empty\nroot:\ndaemon:\n")
        empty_socket = temporary / "empty.sock"
        output = run_foreground(
            binary,
            empty,
            empty_socket,
            script,
            [
                "--root", str(root),
                "--unix-socket", str(empty_socket),
                "--max-accepts", "1",
                "--opcache-dir", str(temporary / "empty-cache"),
            ],
        )
        assert b"\r\n\r\n42" in output

        missing = subprocess.run(
            [str(binary), "-c", str(temporary / "missing.yaml")],
            capture_output=True,
            text=True,
            timeout=5,
        )
        assert missing.returncode == 2
        assert "cannot open configuration file" in missing.stderr

        invalid = temporary / "invalid.yaml"
        invalid.write_text("workers: zero\n")
        rejected = subprocess.run(
            [str(binary), "-c", str(invalid)],
            capture_output=True,
            text=True,
            timeout=5,
        )
        assert rejected.returncode == 2
        assert f"{invalid}:1:" in rejected.stderr
        assert "expected an integer" in rejected.stderr

        invalid_level = temporary / "invalid-level.yaml"
        invalid_level.write_text("log-level: verbose\n")
        rejected_level = subprocess.run(
            [str(binary), "-c", str(invalid_level)],
            capture_output=True,
            text=True,
            timeout=5,
        )
        assert rejected_level.returncode == 2
        assert "expected debug, info, notice, warn, or error" in rejected_level.stderr

        invalid_debug = temporary / "invalid-debug.yaml"
        invalid_debug.write_text("gen-dbg-info: yes\n")
        rejected_debug = subprocess.run(
            [str(binary), "-c", str(invalid_debug)],
            capture_output=True,
            text=True,
            timeout=5,
        )
        assert rejected_debug.returncode == 2
        assert "expected on or off" in rejected_debug.stderr

        obsolete_cli = subprocess.run(
            [str(binary), "--optimize=on"],
            capture_output=True,
            text=True,
            timeout=5,
        )
        assert obsolete_cli.returncode == 2
        assert "unknown or incomplete option" in obsolete_cli.stderr

        removed_worker_connections = subprocess.run(
            [str(binary), "--worker-connections", "2"],
            capture_output=True,
            text=True,
            timeout=5,
        )
        assert removed_worker_connections.returncode == 2
        assert "unknown or incomplete option" in removed_worker_connections.stderr

        obsolete_yaml = temporary / "obsolete.yaml"
        obsolete_yaml.write_text("optimize: on\n")
        rejected_obsolete_yaml = subprocess.run(
            [str(binary), "-c", str(obsolete_yaml)],
            capture_output=True,
            text=True,
            timeout=5,
        )
        assert rejected_obsolete_yaml.returncode == 2
        assert "unknown configuration key 'optimize'" in rejected_obsolete_yaml.stderr

        removed_worker_connections_yaml = temporary / "removed-worker-connections.yaml"
        removed_worker_connections_yaml.write_text("worker-connections: 2\n")
        rejected_worker_connections_yaml = subprocess.run(
            [str(binary), "-c", str(removed_worker_connections_yaml)],
            capture_output=True,
            text=True,
            timeout=5,
        )
        assert rejected_worker_connections_yaml.returncode == 2
        assert "unknown configuration key 'worker-connections'" in rejected_worker_connections_yaml.stderr

        missing_debug_socket = subprocess.run(
            [str(binary), "--debug-enable=on"],
            capture_output=True,
            text=True,
            timeout=5,
        )
        assert missing_debug_socket.returncode == 2
        assert "debug-enable requires --debug-unix-socket" in missing_debug_socket.stderr

        unsafe_debug_tcp = subprocess.run(
            [
                str(binary),
                "--debug-enable=on",
                "--debug-unix-socket",
                str(temporary / "debug.sock"),
                "--debug-tcp-host",
                "0.0.0.0",
                "--debug-tcp-port",
                "19090",
                "--debug-token",
                "token",
            ],
            capture_output=True,
            text=True,
            timeout=5,
        )
        assert unsafe_debug_tcp.returncode == 2
        assert "debug TCP must bind 127.0.0.1" in unsafe_debug_tcp.stderr

        invalid_log = temporary / "invalid-log.yaml"
        write_config(invalid_log, {"log-file": temporary / "missing" / "server.log"})
        rejected_log = subprocess.run(
            [str(binary), "-c", str(invalid_log)],
            capture_output=True,
            text=True,
            timeout=5,
        )
        assert rejected_log.returncode == 2
        assert "create its parent directory and check permissions" in rejected_log.stderr

        daemon_socket = temporary / "daemon.sock"
        daemon_log_base = temporary / "daemon.log"
        daemon_config = temporary / "daemon.yaml"
        write_config(
            daemon_config,
            {
                "root": root,
                "unix-socket": daemon_socket,
                "max-accepts": "1",
                "workers": "1",
                "opcache-dir": temporary / "daemon-cache",
                "log-file": daemon_log_base,
                "log-level": "debug",
                "daemon": "on",
            },
        )
        launched = subprocess.run(
            [str(binary), "-c", str(daemon_config)],
            capture_output=True,
            text=True,
            timeout=5,
        )
        assert launched.returncode == 0
        wait_for_socket(daemon_socket)
        assert b"\r\n\r\n42" in transact(daemon_socket, script)
        for _ in range(200):
            if not daemon_socket.exists():
                break
            time.sleep(0.01)
        assert not daemon_socket.exists(), "daemon did not stop after max-accepts"
        daemon_log_path = Path(f"{daemon_log_base}-{datetime.datetime.now().strftime('%Y%m%d')}")
        daemon_log_text = daemon_log_path.read_text()
        assert "server starting in daemon mode" in daemon_log_text
        assert "fastcgi listening on unix:" in daemon_log_text
    print("fpm config tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
