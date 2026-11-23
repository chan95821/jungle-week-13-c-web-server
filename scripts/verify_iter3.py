#!/usr/bin/env python3
"""Basic Iteration 3 functionality/resource checks, never performance measurements."""
import base64
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import tempfile
import time

from verify_iter1 import SERVER_BIN, pick_free_port, start_server, stop_server
from verify_iter2 import request


def wire(target, method=b"GET"):
    return method + b" " + target + b" HTTP/1.1\r\nHost: localhost\r\n\r\n"


def fd_count(proc):
    return len(list(Path(f"/proc/{proc.pid}/fd").iterdir()))


def wait_reclaimed(proc, baseline):
    for _ in range(100):
        assert proc.poll() is None
        if fd_count(proc) == baseline:
            return
        time.sleep(0.02)
    raise AssertionError("Server file/connection descriptors did not return to baseline")


def main():
    with tempfile.TemporaryDirectory(prefix="web-iter3-") as temporary:
        base = Path(temporary)
        root = base / "www"
        root.mkdir()
        (root / "dir").mkdir()
        (base / "secret.txt").write_bytes(b"outside root")
        (root / "inside-link").symlink_to("page.html")
        (root / "outside-link").symlink_to(base / "secret.txt")
        (root / "dir-link").symlink_to("dir", target_is_directory=True)
        (root / "broken-link").symlink_to("missing")
        os.mkfifo(root / "pipe")
        alias = base / "root-alias"
        alias.symlink_to(root, target_is_directory=True)
        fixtures = {
            "page.html": (b"<!doctype html><title>Static test</title>", b"text/html"),
            "style.CSS": (b"body { color: blue; }", b"text/css"),
            "app.js": (b"console.log('static');", b"text/javascript"),
            "image.png": (base64.b64decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aS1sAAAAASUVORK5CYII="), b"image/png"),
            "image.jpg": (b"\xff\xd8\xff\xe0binary-jpeg-fixture\xff\xd9", b"image/jpeg"),
            "a b.bin": (bytes(range(256)) * 129, b"application/octet-stream"),
            "a+b.bin": (b"literal plus", b"application/octet-stream"),
            "empty": (b"", b"application/octet-stream"),
            "edge": (b"x" * 16384, b"application/octet-stream"),
            "large": (bytes(range(256)) * 1025, b"application/octet-stream"),
            "%2e%2e": (b"one decode only", b"application/octet-stream"),
        }
        for name, (body, _) in fixtures.items():
            (root / name).write_bytes(body)
        for invalid_root in (base / "missing-root", root / "page.html"):
            result = subprocess.run([SERVER_BIN, str(pick_free_port()), str(invalid_root)],
                                    capture_output=True, timeout=3)
            assert result.returncode != 0 and result.stderr

        proc = start_server(pick_free_port(), alias)
        port = int(proc.args[1])
        try:
            for name, (body, content_type) in fixtures.items():
                target = name.replace("%", "%25").replace(" ", "%20").encode("ascii")
                request(port, wire(b"/static/" + target), 200, body, content_type)
            request(port, wire(b"/static/page.html?cache=%2Fignored"), 200,
                    fixtures["page.html"][0], b"text/html")
            errors = {
                b"/static": 404, b"/static/": 403, b"/static/dir": 403,
                b"/static/dir/": 403, b"/static/missing": 404,
                b"/static/page.html/child": 404, b"/static/a//b": 400,
                b"/static/../secret.txt": 403, b"/static/%2e%2e/secret.txt": 403,
                b"/static/%2Fetc": 400, b"/static/a%5Cb": 400,
                b"/static/a%00b": 400, b"/static/%1F": 400,
                b"/static/inside-link": 403, b"/static/outside-link": 403,
                b"/static/dir-link/child": 403, b"/static/broken-link": 403,
                b"/static/pipe": 403,
            }
            name_max = os.pathconf(root, "PC_NAME_MAX")
            if 0 < name_max < 8000:
                errors[b"/static/" + b"a" * (name_max + 1)] = 414
            for target, status in errors.items():
                request(port, wire(target), status)
            for target in (b"/static/page.html", b"/static/missing", b"/greet"):
                headers = request(port, wire(target, b"POST"), 405)
                assert headers[b"allow"] == b"GET"
            for target in (b"/", b"/hello"):
                request(port, wire(target), 200, b"Hello", b"text/plain")
            body = b"binary\x00\xff"
            request(port, b"POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 8\r\n\r\n" + body,
                    200, body, b"application/octet-stream")
            for target, body in (
                (b"/greet", b"Hello, World!\n"),
                (b"/greet?name=Alice", b"Hello, Alice!\n"),
                (b"/greet?name=Bob", b"Hello, Bob!\n"),
                (b"/greet?na%6de=A%26B%3DC", b"Hello, A&B=C!\n"),
                (b"/greet?name=A+B", b"Hello, A B!\n"),
                (b"/greet?name=%ED%95%9C", "Hello, 한!\n".encode()),
                (b"/greet?unknown=valid", b"Hello, World!\n"),
            ):
                request(port, wire(target), 200, body, b"text/plain; charset=utf-8")
            for target in (b"/greet?name=", b"/greet?name=A&na%6de=B",
                           b"/greet?name=%00", b"/greet?x=%FF",
                           b"/greet?%C2%80=x", b"/greet?name=%ED%A0%80",
                           b"/greet?name=%G0"):
                request(port, wire(target), 400)

            # Permission tests use disposable fixtures only, never project chmod.
            restricted = root / "restricted"
            restricted.write_bytes(b"not readable")
            restricted.chmod(0)
            blocked_root = base / "blocked"
            blocked_root.mkdir()
            blocked_root.chmod(0)
            try:
                if os.geteuid() != 0 and not os.access(restricted, os.R_OK):
                    request(port, wire(b"/static/restricted"), 403)
                else:
                    print("[SKIP] effective credentials bypass file permission fixture")
                if os.geteuid() != 0 and not os.access(blocked_root, os.R_OK):
                    result = subprocess.run([SERVER_BIN, str(pick_free_port()), str(blocked_root)],
                                            capture_output=True, timeout=3)
                    assert result.returncode != 0
                else:
                    print("[SKIP] effective credentials bypass root permission fixture")
            finally:
                restricted.chmod(0o600)
                blocked_root.chmod(0o700)

            time.sleep(0.05)  # Let the preceding close complete; no timing metric.
            baseline = fd_count(proc)
            for _ in range(3):
                request(port, wire(b"/static/large"), 200, fixtures["large"][0])
                request(port, wire(b"/static/outside-link"), 403)
                with socket.create_connection(("127.0.0.1", port), timeout=3) as client:
                    client.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
                    client.sendall(wire(b"/static/large"))
                request(port, wire(b"/hello"), 200, b"Hello")
                wait_reclaimed(proc, baseline)
        finally:
            assert stop_server(proc) == 0
        # Configured root symlink resolves once; missing/file roots already rejected.
        print("Basic Iteration 3 checks passed; mutation/fault sender checks are in test_file_stream.")
        print("No throughput, latency or performance comparison measurements collected.")


if __name__ == "__main__":
    main()