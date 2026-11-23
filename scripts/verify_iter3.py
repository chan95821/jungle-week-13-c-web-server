#!/usr/bin/env python3
"""사용자가 확정한 정적 파일 기본 검증: HTML/CSS/이미지와 403/404만 실행한다."""
import os
from pathlib import Path
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
    # 외부 이미지 도구 없이 유효한 1x1 RGBA PNG 바이트를 만든다.
    def chunk(kind, data):
        return struct.pack("!I", len(data)) + kind + data + struct.pack("!I", zlib.crc32(kind + data))
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack("!IIBBBBB", 1, 1, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(b"\x00\x20\x80\x40\xff"))
            + chunk(b"IEND", b""))


class StaticBasicTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="week13-static-basic-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.root = Path(cls.temp.name) / "documents"
        Path(cls.temp.name).chmod(0o755)
        shutil.copytree(PROJECT / "public", cls.root)
        cls.root.chmod(0o755)
        cls.png = png_sample()
        (cls.root / "pixel.png").write_bytes(cls.png)
        (cls.root / "denied.txt").write_bytes(b"must not be served")
        (cls.root / "denied.txt").chmod(0)
        cls.port = pick_free_port()
        options = {}
        # root로 실행하면 읽기 권한을 우회하므로 서버만 비루트 UID로 실행한다.
        if os.geteuid() == 0:
            options.update(user=65534, group=65534, extra_groups=[])
        cls.process = subprocess.Popen(
            [SERVER_BIN, str(cls.port), str(cls.root)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, **options)
        cls.addClassCleanup(cls.stop_server)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if cls.process.poll() is not None:
                raise AssertionError(f"서버 시작 실패: {cls.process.communicate()}")
            try:
                with socket.create_connection(("127.0.0.1", cls.port), timeout=.2):
                    return
            except OSError:
                time.sleep(.02)
        raise AssertionError("서버 시작 대기 초과")

    @classmethod
    def stop_server(cls):
        if cls.process.poll() is None:
            cls.process.send_signal(signal.SIGTERM)
        try:
            output, errors = cls.process.communicate(timeout=7)
        except subprocess.TimeoutExpired:
            cls.process.kill()
            cls.process.communicate()
            raise AssertionError("서버 종료 대기 초과")
        assert cls.process.returncode == 0, (output, errors)

    def exchange(self, path, expected_status, expected_body, expected_type):
        with socket.create_connection(("127.0.0.1", self.port), timeout=7) as client:
            client.sendall(b"GET " + path + b" HTTP/1.1\r\nHost: localhost\r\n\r\n")
            client.shutdown(socket.SHUT_WR)
            reader = ResponseReader(client)
            status, fields, body = reader.read()
            self.assertEqual(status, expected_status)
            self.assertEqual(fields[b"content-type"], expected_type)
            self.assertEqual(int(fields[b"content-length"]), len(body))
            self.assertEqual(fields[b"connection"], b"close")
            self.assertEqual(body, expected_body)
            reader.assert_closed()

    def test_html_css_images(self):
        cases = [
            (b"/static/", "index.html", b"text/html; charset=utf-8"),
            (b"/static/css/style.css", "css/style.css", b"text/css; charset=utf-8"),
            (b"/static/images/jungle.svg", "images/jungle.svg", b"image/svg+xml"),
            (b"/static/pixel.png", "pixel.png", b"image/png"),
        ]
        for url, name, mime in cases:
            with self.subTest(url=url):
                self.exchange(url, 200, (self.root / name).read_bytes(), mime)

    def test_forbidden_403(self):
        self.exchange(b"/static/denied.txt", 403, b"Forbidden", b"text/plain")

    def test_not_found_404(self):
        self.exchange(b"/static/missing.html", 404, b"Not Found", b"text/plain")


if __name__ == "__main__":
    unittest.main(verbosity=2)
