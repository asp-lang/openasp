#!/usr/bin/env python3
"""Build a public-header-only extension and exercise the CLI loader."""

from __future__ import annotations

import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(next(parent for parent in Path(__file__).resolve().parents if (parent / "Makefile").is_file()) / "tests/integration/fastcgi"))
from test_fastcgi import start_server, transact

def main() -> int:
    root = next(parent for parent in Path(__file__).resolve().parents if (parent / "Makefile").is_file())
    openasp = Path(sys.argv[1]).resolve()
    openasp_fpm = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else None
    example = root / "examples/extensions/acme_math"
    subprocess.run(["make", "-C", str(example)], check=True, capture_output=True, text=True)
    library = example / "libacme_math.dylib"
    digest = hashlib.sha256(library.read_bytes()).hexdigest()
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
        if openasp_fpm:
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
        bad = config.with_name("bad.yaml")
        bad.write_text(config.read_text().replace("Acme.Math", "Acme.Unknown"))
        rejected = subprocess.run([str(openasp), "-c", str(bad),
                                   str(root / "tests/fixtures/common/external_math.asp")],
                                 capture_output=True, text=True, timeout=10)
        assert rejected.returncode != 0
        assert "external component initialization failed" in rejected.stderr, rejected.stderr
    subprocess.run(["make", "-C", str(example), "clean"], check=True, capture_output=True, text=True)
    print("extension SDK tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
