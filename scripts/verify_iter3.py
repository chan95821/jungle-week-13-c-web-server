#!/usr/bin/env python3
"""정적 파일 기능 검증. 임시 Linux 파일시스템에서 실행하며 성능 수치는 수집하지 않는다."""
import os
from pathlib import Path
import resource
import shutil
import signal
import socket
import struct
import subprocess
import tempfile
import time
import unittest
import zlib

from verify_iter1 import SERVER_BIN, pick_free_port
from verify_iter2 import ResponseReader

PROJECT = Path(__file__).resolve().parent.parent


def png_sample():
    # 유효한 1x1 RGBA PNG를 표준 라이브러리로 만든다. 이미지 편집 도구에 의존하지 않는다.
    def chunk(kind, data):
        return struct.pack("!I", len(data)) + kind + data + struct.pack("!I", zlib.crc32(kind + data))
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack("!IIBBBBB", 1, 1, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(b"\x00\x20\x80\x40\xff"))
            + chunk(b"IEND", b""))


def child_file_limit():
    # 0/1/2, rootfd, listenfd, connfd, 탐색용 복제 FD 다음 openat에서 EMFILE 발생.
    # 부모/호스트의 제한은 바꾸지 않는 작은 오류 주입이다. sanitizer 프로세스에는 적용하지 않는다.
    resource.setrlimit(resource.RLIMIT_NOFILE, (7, 7))


class StaticTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="week13-static-e2e-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.base = Path(cls.temp.name)
        cls.base.chmod(0o755)
        cls.root = cls.base / "documents"
        shutil.copytree(PROJECT / "public", cls.root)
        cls.root.chmod(0o755)
        cls.png = png_sample()
        (cls.root / "pixel.png").write_bytes(cls.png)
        (cls.root / "empty.bin").write_bytes(b"")
        cls.binary = bytes(range(256)) * 1025
        (cls.root / "large.bin").write_bytes(cls.binary)
        (cls.root / "a b.txt").write_bytes(b"space")
        (cls.root / "a+b.txt").write_bytes(b"plus")
        (cls.root / "%2e%2e").write_bytes(b"one decode only")
        for size in (16383, 16384, 16385):
            (cls.root / f"size-{size}.bin").write_bytes(b"X" * size)
        (cls.root / "denied.txt").write_bytes(b"must not be served")
        (cls.root / "denied.txt").chmod(0)
        (cls.root / "locked").mkdir()
        (cls.root / "locked" / "secret.txt").write_bytes(b"secret")
        (cls.root / "locked").chmod(0)
        cls.addClassCleanup((cls.root / "locked").chmod, 0o700)
        outside = cls.base / "outside.txt"
        outside.write_bytes(b"outside sentinel")
        (cls.root / "outside-link").symlink_to(outside)
        (cls.root / "internal-link").symlink_to("index.html")
        (cls.root / "dir-link").symlink_to("css", target_is_directory=True)
        os.mkfifo(cls.root / "pipe")
        cls.port = pick_free_port()
        cls.process = cls.spawn(cls.port)
        cls.addClassCleanup(cls.stop, cls.process)

    @classmethod
    def spawn(cls, port, limited=False):
        options = {}
        # 검증기를 root로 실행해도 서버는 비루트로 실행해 chmod 000의 403을 확인한다.
        if os.geteuid() == 0:
            options.update(user=65534, group=65534, extra_groups=[])
        if limited:
            options["preexec_fn"] = child_file_limit
        process = subprocess.Popen(
            [SERVER_BIN, str(port), str(cls.root)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, **options)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise AssertionError(f"서버 시작 실패: {process.communicate()}")
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=.2):
                    return process
            except OSError:
                time.sleep(.02)
        process.kill()
        process.communicate()
        raise AssertionError("서버 시작 대기 초과")

    @staticmethod
    def stop(process):
        if process.poll() is None:
            process.send_signal(signal.SIGTERM)
        try:
            output, errors = process.communicate(timeout=7)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate()
            raise AssertionError("서버 종료 대기 초과")
        assert process.returncode == 0, (output, errors)
        assert b"Sanitizer" not in errors and b"runtime error:" not in errors, errors

    def exchange(self, path, status=200, expected=None, mime=None,
                 method=b"GET", body=b"", fragments=False, port=None):
        port = self.port if port is None else port
        wire = (method + b" " + path + b" HTTP/1.1\r\nHost: localhost\r\nContent-Length: "
                + str(len(body)).encode() + b"\r\n\r\n" + body)
        with socket.create_connection(("127.0.0.1", port), timeout=7) as client:
            if fragments:
                for part in (wire[:2], wire[2:-3], wire[-3:]):
                    client.sendall(part)
                    time.sleep(.01)
            else:
                client.sendall(wire)
            client.shutdown(socket.SHUT_WR)
            reader = ResponseReader(client)
            actual, fields, received = reader.read()
            self.assertEqual(actual, status, (path, received))
            self.assertEqual(int(fields[b"content-length"]), len(received))
            self.assertEqual(fields[b"connection"], b"close")
            if expected is not None:
                self.assertEqual(received, expected)
            if mime is not None:
                self.assertEqual(fields[b"content-type"], mime)
            reader.assert_closed()
            return fields, received

    def test_html_css_svg_png(self):
        cases = [
            (b"/static/", "index.html", b"text/html; charset=utf-8"),
            (b"/static/index.html?version=1", "index.html", b"text/html; charset=utf-8"),
            (b"/static/css/style.css", "css/style.css", b"text/css; charset=utf-8"),
            (b"/static/images/jungle.svg", "images/jungle.svg", b"image/svg+xml"),
            (b"/static/pixel.png", "pixel.png", b"image/png"),
        ]
        for url, name, mime in cases:
            with self.subTest(url=url):
                self.exchange(url, expected=(self.root / name).read_bytes(), mime=mime)

    def test_empty_large_and_buffer_boundaries(self):
        for name in ("empty.bin", "large.bin", "size-16383.bin", "size-16384.bin", "size-16385.bin"):
            with self.subTest(name=name):
                self.exchange(b"/static/" + name.encode(), expected=(self.root / name).read_bytes(),
                              mime=b"application/octet-stream")

    def test_path_decoding(self):
        for path, expected in [
            (b"/static/a%20b.txt", b"space"), (b"/static/a+b.txt", b"plus"),
            (b"/static/%252e%252e", b"one decode only"),
        ]:
            self.exchange(path, expected=expected)

    def test_bad_path_400(self):
        for path in (b"/static/a%00b", b"/static/a%0ab", b"/static/a%5cb", b"/static/a%GG", b"/static//a"):
            with self.subTest(path=path):
                self.exchange(path, 400)

    def test_forbidden_403(self):
        for path in (
            b"/static/../outside.txt", b"/static/%2e%2e/outside.txt", b"/static/css%2f..%2findex.html",
            b"/static/outside-link", b"/static/internal-link", b"/static/dir-link/style.css",
            b"/static/css", b"/static/css/", b"/static/pipe", b"/static/denied.txt", b"/static/locked/secret.txt",
        ):
            with self.subTest(path=path):
                _, received = self.exchange(path, 403)
                self.assertEqual(received, b"Forbidden")

    def test_not_found_404(self):
        for path in (b"/static/missing.html", b"/static/missing/file", b"/static/index.html/file", b"/static"):
            self.exchange(path, 404, b"Not Found")

    def test_methods_and_previous_routes(self):
        fields, _ = self.exchange(b"/static/index.html", 405, method=b"POST", body=b"x")
        self.assertEqual(fields[b"allow"], b"GET")
        for path in (b"/", b"/hello"):
            self.exchange(path, expected=b"Hello", mime=b"text/plain")
        self.exchange(b"/echo", expected=b"a\0b", mime=b"application/octet-stream", method=b"POST", body=b"a\0b")

    def test_fragmented_get_body(self):
        self.exchange(b"/static/pixel.png", expected=self.png, body=b"a\0b", fragments=True)

    def test_expect_and_early_file_error(self):
        for path, expected_status in [(b"/static/pixel.png", 100), (b"/static/missing", 404)]:
            wire = b"GET " + path + b" HTTP/1.1\r\nHost: a\r\nContent-Length: 3\r\nExpect: 100-continue\r\n\r\n"
            with socket.create_connection(("127.0.0.1", self.port), timeout=7) as client:
                client.sendall(wire)
                reader = ResponseReader(client)
                status, _, received = reader.read()
                self.assertEqual(status, expected_status)
                if status == 100:
                    client.sendall(b"abc")
                    client.shutdown(socket.SHUT_WR)
                    status, _, received = reader.read()
                    self.assertEqual((status, received), (200, self.png))
                else:
                    client.shutdown(socket.SHUT_WR)
                reader.assert_closed()

    def test_curl_files_and_traversal(self):
        for path, expected in [("/static/pixel.png", self.png), ("/static/../outside.txt", b"Forbidden")]:
            result = subprocess.run(
                ["curl", "--noproxy", "*", "--path-as-is", "-sS", "--max-time", "7",
                 "-w", "\n%{http_code}", f"http://127.0.0.1:{self.port}{path}"],
                capture_output=True, timeout=9, check=True)
            body, status = result.stdout.rsplit(b"\n", 1)
            self.assertEqual(body, expected)
            self.assertEqual(status, b"200" if path.endswith("png") else b"403")

    def test_client_reset_and_recovery(self):
        with socket.create_connection(("127.0.0.1", self.port), timeout=7) as client:
            client.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
            client.sendall(b"GET /static/large.bin HTTP/1.1\r\nHost: a\r\n\r\n")
        self.exchange(b"/static/pixel.png", expected=self.png)

    def test_startup_errors(self):
        for root in (self.base / "missing", self.root / "index.html"):
            result = subprocess.run([SERVER_BIN, str(pick_free_port()), str(root)],
                                    capture_output=True, timeout=5)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b"document root", result.stderr)

    def test_internal_file_open_error_500(self):
        if os.environ.get("STATIC_SKIP_FD_LIMIT") == "1":
            self.skipTest("sanitizer의 내부 FD 사용과 간섭하므로 FD 제한 주입은 일반 빌드에서 검증")
        port = pick_free_port()
        process = self.spawn(port, limited=True)
        try:
            self.exchange(b"/static/index.html", 500, b"Internal Server Error", port=port)
            self.exchange(b"/hello", expected=b"Hello", port=port)
        finally:
            self.stop(process)


if __name__ == "__main__":
    unittest.main(verbosity=2)
