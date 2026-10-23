#!/usr/bin/env python3
"""Build a public-header-only extension and exercise the CLI loader."""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, str(next(parent for parent in Path(__file__).resolve().parents if (parent / "Makefile").is_file()) / "tests/integration/fastcgi"))
from test_fastcgi import start_server, transact

def main() -> int:
    root = next(parent for parent in Path(__file__).resolve().parents if (parent / "Makefile").is_file())
    openasp = Path(sys.argv[1]).resolve()
    openasp_fpm = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else None
    example = root / "examples/extensions/acme_math"
    ai_example = root / "extensions/openasp-ai"
    subprocess.run(["make", "-C", str(example)], check=True, capture_output=True, text=True)
    subprocess.run(["make", "-C", str(ai_example)], check=True, capture_output=True, text=True)
    library = example / "libacme_math.dylib"
    ai_library = ai_example / (
        "libopenasp_ai.dylib" if sys.platform == "darwin" else "libopenasp_ai.so"
    )
    digest = hashlib.sha256(library.read_bytes()).hexdigest()
    ai_digest = hashlib.sha256(ai_library.read_bytes()).hexdigest()
    with tempfile.TemporaryDirectory(prefix="openasp-extension-") as directory:
        config = Path(directory) / "openasp.yaml"
        config.write_text(
            "external-components:\n"
            "  enabled: on\n"
            "  require-sha256: on\n"
            "  modules:\n"
            "    - id: acme-math\n"
            f"      library: {library}\n"
            "      mode: in-process\n"
            f"      sha256: {digest}\n"
            "      prog-ids:\n"
            "        - Acme.Math\n"
        )
        oac = Path(directory) / "external-math.oac"
        result = subprocess.run(
            [str(openasp), "-c", str(config), str(root / "tests/fixtures/common/external_math.asp"),
             "--root", str(root / "tests/fixtures"), "--gc-diagnostics", "--emit-oac", str(oac)],
            capture_output=True, text=True, timeout=10,
        )
        assert result.returncode == 0, f"returncode={result.returncode}\n{result.stderr}\n{result.stdout}"
        assert result.stdout.strip() == "1.0.0:42:84", result.stdout
        assert "OpenASP GC:" in result.stderr, result.stderr
        assert oac.is_file() and oac.stat().st_size > 0, oac
        callback = subprocess.run(
            [str(openasp), "-c", str(config), "--runtime-mode=batch", str(root / "tests/fixtures/common/external_callback.asp"),
             "--root", str(root / "tests/fixtures")],
            capture_output=True, text=True, timeout=10,
        )
        assert callback.returncode == 0, f"callback={callback.returncode}\n{callback.stderr}\n{callback.stdout}"
        assert callback.stdout.strip() == "2", callback.stdout
        v2 = subprocess.run(
            [str(openasp), "-c", str(config), str(root / "tests/fixtures/common/external_v2.asp"),
             "--root", str(root / "tests/fixtures")],
            capture_output=True, text=True, timeout=10,
        )
        assert v2.returncode == 0, f"v2={v2.returncode}\n{v2.stderr}\n{v2.stdout}"
        assert v2.stdout.strip() == "42|123|abc|4|42|12|3", v2.stdout
        isolated_config = Path(directory) / "openasp-isolated.yaml"
        isolated_config.write_text(config.read_text().replace("mode: in-process", "mode: isolated"))
        isolated = subprocess.run(
            [str(openasp), "-c", str(isolated_config), str(root / "tests/fixtures/common/external_v2.asp"),
             "--root", str(root / "tests/fixtures")],
            capture_output=True, text=True, timeout=10,
        )
        assert isolated.returncode == 0, f"isolated={isolated.returncode}\n{isolated.stderr}\n{isolated.stdout}"
        assert isolated.stdout.strip() == "42|123|abc|4|42|12|3", isolated.stdout
        ai_config = Path(directory) / "openasp-ai.yaml"
        ai_config.write_text(
            "external-components:\n"
            "  enabled: on\n"
            "  require-sha256: on\n"
            "  modules:\n"
            "    - id: openasp-ai\n"
            f"      library: {ai_library}\n"
            "      mode: in-process\n"
            f"      sha256: {ai_digest}\n"
            "      prog-ids:\n"
            "        - OpenASP.AI\n"
        )
        ai_command = [
            str(openasp), "-c", str(ai_config),
            str(root / "tests/fixtures/common/external_ai.asp"),
            "--root", str(root / "tests/fixtures"),
        ]
        ai_in_process = subprocess.run(
            ai_command, capture_output=True, text=True, timeout=10,
        )
        assert ai_in_process.returncode == 0, ai_in_process.stderr
        assert ai_in_process.stdout.strip() == "12|43|50|2|6|2x3", ai_in_process.stdout
        ai_isolated_config = Path(directory) / "openasp-ai-isolated.yaml"
        ai_isolated_config.write_text(
            ai_config.read_text().replace("mode: in-process", "mode: isolated")
        )
        ai_isolated_command = ai_command.copy()
        ai_isolated_command[2] = str(ai_isolated_config)
        ai_isolated = subprocess.run(
            ai_isolated_command, capture_output=True, text=True, timeout=10,
        )
        assert ai_isolated.returncode == 0, ai_isolated.stderr
        assert ai_isolated.stdout.strip() == "12|43|50|2|6|2x3", ai_isolated.stdout
        ai_full_command = ai_command.copy()
        ai_full_command[3] = str(root / "tests/fixtures/common/external_ai_full.asp")
        ai_full = subprocess.run(
            ai_full_command, capture_output=True, text=True, timeout=10,
        )
        assert ai_full.returncode == 0, ai_full.stderr
        assert ai_full.stdout.strip().endswith("|4|6|1"), ai_full.stdout
        ai_full_isolated_command = ai_full_command.copy()
        ai_full_isolated_command[2] = str(ai_isolated_config)
        ai_full_isolated = subprocess.run(
            ai_full_isolated_command, capture_output=True, text=True, timeout=10,
        )
        assert ai_full_isolated.returncode == 0, ai_full_isolated.stderr
        assert ai_full_isolated.stdout.strip() == ai_full.stdout.strip()
        for data_config in (ai_config, ai_isolated_config):
            ai_data = subprocess.run(
                [
                    str(openasp), "-c", str(data_config),
                    str(root / "tests/fixtures/common/external_ai_data.asp"),
                    "--root", str(root / "tests/fixtures"),
                ],
                capture_output=True, text=True, timeout=10,
            )
            assert ai_data.returncode == 0, ai_data.stderr
            assert ai_data.stdout.strip() == "4|True|2|2|2|3|hello world", ai_data.stdout
        for trace_config in (ai_config, ai_isolated_config):
            ai_trace = subprocess.run(
                [
                    str(openasp), "-c", str(trace_config),
                    str(root / "tests/fixtures/common/external_ai_trace.asp"),
                    "--root", str(root / "tests/fixtures"),
                ],
                capture_output=True, text=True, timeout=10,
            )
            assert ai_trace.returncode == 0, ai_trace.stderr
            prefix, value = ai_trace.stdout.strip().split("|", 1)
            assert prefix == "1x1" and float(value) == float(value), ai_trace.stdout
        for persistence_config in (ai_config, ai_isolated_config):
            ai_persistence = subprocess.run(
                [
                    str(openasp), "-c", str(persistence_config),
                    str(root / "tests/fixtures/common/external_ai_persistence.asp"),
                    "--root", str(root / "tests/fixtures"),
                ],
                capture_output=True, text=True, timeout=10,
            )
            assert ai_persistence.returncode == 0, ai_persistence.stderr
            assert ai_persistence.stdout.strip() == "3", ai_persistence.stdout
        for runtime_config in (ai_config, ai_isolated_config):
            ai_runtime = subprocess.run(
                [
                    str(openasp), "-c", str(runtime_config),
                    str(root / "tests/fixtures/common/external_ai_runtime.asp"),
                    "--root", str(root / "tests/fixtures"),
                ],
                capture_output=True, text=True, timeout=10,
            )
            assert ai_runtime.returncode == 0, ai_runtime.stderr
            assert ai_runtime.stdout.strip() == (
                "2|weight|buffer|True|1|6|1|weight|1|cpu:0|1|cpu|True|True|3|9|0.125|dog"
            ), ai_runtime.stdout
        with ThreadPoolExecutor(max_workers=4) as executor:
            ai_concurrent = list(executor.map(
                lambda _: subprocess.run(
                    ai_isolated_command, capture_output=True, text=True, timeout=10
                ),
                range(8),
            ))
        assert all(item.returncode == 0 for item in ai_concurrent), [
            item.stderr for item in ai_concurrent if item.returncode
        ]
        assert all(item.stdout.strip() == "12|43|50|2|6|2x3" for item in ai_concurrent)
        ai_limited_config = Path(directory) / "openasp-ai-limited.yaml"
        ai_limited_config.write_text(
            ai_config.read_text().replace(
                "      mode: in-process\n",
                "      mode: in-process\n      max-model-external-bytes: 16\n",
            )
        )
        ai_limited_command = ai_command.copy()
        ai_limited_command[2] = str(ai_limited_config)
        ai_limited = subprocess.run(
            ai_limited_command, capture_output=True, text=True, timeout=10,
        )
        assert ai_limited.returncode != 0
        assert "resource budget exceeded" in ai_limited.stderr, ai_limited.stderr
        persistence = subprocess.run(
            [str(openasp), "-c", str(isolated_config),
             str(root / "tests/fixtures/common/external_persistence.asp"),
             "--root", str(root / "tests/fixtures")],
            capture_output=True, text=True, timeout=10,
        )
        assert persistence.returncode == 0, persistence.stderr
        assert persistence.stdout.strip() == "42", persistence.stdout
        deadline = subprocess.run(
            [str(openasp), "-c", str(isolated_config),
             str(root / "tests/fixtures/common/external_deadline.asp"),
             "--root", str(root / "tests/fixtures")],
            capture_output=True, text=True, timeout=10,
        )
        assert deadline.returncode == 0 and deadline.stdout.strip() == "True", (
            deadline.stderr, deadline.stdout
        )
        command = [
            str(openasp), "-c", str(isolated_config),
            str(root / "tests/fixtures/common/external_v2.asp"),
            "--root", str(root / "tests/fixtures"),
        ]
        with ThreadPoolExecutor(max_workers=6) as executor:
            concurrent = list(executor.map(
                lambda _: subprocess.run(command, capture_output=True, text=True, timeout=10),
                range(12),
            ))
        assert all(item.returncode == 0 for item in concurrent), [
            item.stderr for item in concurrent if item.returncode
        ]
        assert all(item.stdout.strip() == "42|123|abc|4|42|12|3" for item in concurrent)
        timeout_config = Path(directory) / "openasp-timeout.yaml"
        timeout_config.write_text(
            isolated_config.read_text().replace(
                "      mode: isolated\n", "      mode: isolated\n      call-timeout-ms: 50\n"
            )
        )
        blocked = subprocess.run(
            [str(openasp), "-c", str(timeout_config),
             str(root / "tests/fixtures/common/external_block.asp"),
             "--root", str(root / "tests/fixtures")],
            capture_output=True, text=True, timeout=5,
        )
        assert blocked.returncode != 0 and "timed out" in blocked.stderr, blocked.stderr
        crashed = subprocess.run(
            [str(openasp), "-c", str(isolated_config),
             str(root / "tests/fixtures/common/external_crash.asp"),
             "--root", str(root / "tests/fixtures")],
            capture_output=True, text=True, timeout=5,
        )
        assert crashed.returncode != 0 and "host terminated" in crashed.stderr, crashed.stderr
        missing_host_environment = os.environ.copy()
        missing_host_environment["OPENASP_COMPONENT_HOST"] = str(Path(directory) / "missing-host")
        missing_host = subprocess.run(
            command, capture_output=True, text=True, timeout=5, env=missing_host_environment,
        )
        assert missing_host.returncode != 0
        assert "unable to execute isolated component host" in missing_host.stderr, missing_host.stderr
        compiler = os.environ.get("CC", "cc")
        bad_abi_library = Path(directory) / "libacme_bad_abi.dylib"
        capability_library = Path(directory) / "libacme_network.dylib"
        common_compile = [
            compiler, "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wpedantic",
            "-fPIC", "-shared", f"-I{root / 'include'}", str(example / "acme_math.c"),
        ]
        subprocess.run(
            common_compile + ["-DACME_ABI_MAJOR=1", "-o", str(bad_abi_library)],
            check=True, capture_output=True, text=True,
        )
        subprocess.run(
            common_compile + ["-DACME_REQUIRED_CAPABILITIES=4", "-o", str(capability_library)],
            check=True, capture_output=True, text=True,
        )
        bad_abi_config = Path(directory) / "bad-abi.yaml"
        bad_abi_config.write_text(
            isolated_config.read_text()
            .replace(str(library), str(bad_abi_library))
            .replace(f"      sha256: {digest}\n", "")
            .replace("  require-sha256: on", "  require-sha256: off")
        )
        bad_abi = subprocess.run(
            [str(openasp), "-c", str(bad_abi_config),
             str(root / "tests/fixtures/common/external_math.asp")],
            capture_output=True, text=True, timeout=5,
        )
        assert bad_abi.returncode != 0 and "failed to initialize" in bad_abi.stderr, bad_abi.stderr
        capability_config = Path(directory) / "missing-capability.yaml"
        capability_config.write_text(
            bad_abi_config.read_text()
            .replace(str(bad_abi_library), str(capability_library))
        )
        capability = subprocess.run(
            [str(openasp), "-c", str(capability_config),
             str(root / "tests/fixtures/common/external_math.asp")],
            capture_output=True, text=True, timeout=5,
        )
        assert capability.returncode != 0 and "failed to initialize" in capability.stderr, capability.stderr
        if openasp_fpm:
            ai_socket_path = Path(directory) / "extension-ai.sock"
            ai_fpm_config = Path(directory) / "openasp-ai-fpm.yaml"
            ai_fpm_config.write_text(
                f"root: {root / 'tests/fixtures'}\n"
                f"unix-socket: {ai_socket_path}\n"
                "max-accepts: 1\nworkers: 1\n"
                f"opcache-dir: {Path(directory) / 'ai-cache'}\n"
                + ai_isolated_config.read_text()
            )
            server = start_server([str(openasp_fpm), "-c", str(ai_fpm_config)], ai_socket_path)
            try:
                response = transact(ai_socket_path, {
                    "SCRIPT_FILENAME": str(root / "tests/fixtures/common/external_ai.asp"),
                    "SCRIPT_NAME": "/external_ai.asp",
                    "REQUEST_METHOD": "GET", "QUERY_STRING": "",
                    "SERVER_NAME": "localhost", "SERVER_PORT": "80",
                })
                assert b"Status: 200 OK" in response, response
                assert response.split(b"\r\n\r\n", 1)[1].strip() == b"12|43|50|2|6|2x3"
                assert server.wait(timeout=5) == 0
            finally:
                if server.poll() is None:
                    server.terminate()
                    server.wait(timeout=5)
            socket_path = Path(directory) / "extension.sock"
            fpm_config = Path(directory) / "openasp-fpm.yaml"
            fpm_config.write_text(
                f"root: {root / 'tests/fixtures'}\n"
                f"unix-socket: {socket_path}\n"
                "max-accepts: 1\nworkers: 1\n"
                f"opcache-dir: {Path(directory) / 'cache'}\n"
                + config.read_text()
            )
            server = start_server([str(openasp_fpm), "-c", str(fpm_config)], socket_path)
            try:
                response = transact(socket_path, {
                    "SCRIPT_FILENAME": str(root / "tests/fixtures/common/external_callback.asp"),
                    "SCRIPT_NAME": "/external_callback.asp",
                    "REQUEST_METHOD": "GET", "QUERY_STRING": "", "SERVER_NAME": "localhost", "SERVER_PORT": "80",
                })
                assert b"Status: 200 OK" in response and response.endswith(b"\r\n\r\n2\n"), response
                assert server.wait(timeout=5) == 0
            finally:
                if server.poll() is None:
                    server.terminate()
                    server.wait(timeout=5)
            recovery_socket = Path(directory) / "extension-recovery.sock"
            recovery_config = Path(directory) / "openasp-fpm-recovery.yaml"
            recovery_config.write_text(
                f"root: {root / 'tests/fixtures'}\n"
                f"unix-socket: {recovery_socket}\n"
                "max-accepts: 2\nworkers: 1\n"
                f"opcache-dir: {Path(directory) / 'recovery-cache'}\n"
                + isolated_config.read_text()
            )
            server = start_server(
                [str(openasp_fpm), "-c", str(recovery_config)], recovery_socket
            )
            request = {
                "SCRIPT_FILENAME": str(root / "tests/fixtures/common/external_crash.asp"),
                "SCRIPT_NAME": "/external_crash.asp",
                "REQUEST_METHOD": "GET", "QUERY_STRING": "",
                "SERVER_NAME": "localhost", "SERVER_PORT": "80",
            }
            try:
                crash_response = transact(recovery_socket, request)
                assert b"Status: 500 Internal Server Error" in crash_response, crash_response
                request["SCRIPT_FILENAME"] = str(
                    root / "tests/fixtures/common/external_v2.asp"
                )
                request["SCRIPT_NAME"] = "/external_v2.asp"
                recovered = transact(recovery_socket, request)
                assert b"Status: 200 OK" in recovered, recovered
                assert recovered.split(b"\r\n\r\n", 1)[1].strip() == (
                    b"42|123|abc|4|42|12|3"
                ), recovered
                assert server.wait(timeout=5) == 0
            finally:
                if server.poll() is None:
                    server.terminate()
                    server.wait(timeout=5)
        bad = config.with_name("bad.yaml")
        bad.write_text(config.read_text().replace("Acme.Math", "Acme.Unknown"))
        rejected = subprocess.run([str(openasp), "-c", str(bad),
                                   str(root / "tests/fixtures/common/external_math.asp")],
                                 capture_output=True, text=True, timeout=10)
        assert rejected.returncode != 0
        assert "external component initialization failed" in rejected.stderr, rejected.stderr
    subprocess.run(["make", "-C", str(example), "clean"], check=True, capture_output=True, text=True)
    subprocess.run(["make", "-C", str(ai_example), "clean"], check=True, capture_output=True, text=True)
    print("extension SDK tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
