#!/usr/bin/env python3
"""실제 서버 프로세스로 HTTP 경계/본문/오류 복구를 검증한다.

소켓 sendall의 호출 횟수와 서버 recv의 호출 횟수는 일치하지 않는다.
따라서 일부 테스트는 전송 중간에 응답이 없는지 직접 확인하고, 단위 테스트는
이미 받은 본문의 모든 분할 위치를 결정적으로 검사한다.
"""
import os
import signal
import socket
import struct
import subprocess
import time
import unittest

SERVER = os.environ.get("SERVER_BIN", "./server")
BODY_LIMIT = 1024 * 1024


def read_response(client):
    # recv 한 번에 응답이 전부 도착한다는 가정을 하지 않는다.
    chunks = []
    while True:
        chunk = client.recv(65536)
        if not chunk:
            break
        chunks.append(chunk)
    raw = b"".join(chunks)
    head, body = raw.split(b"\r\n\r\n", 1)
    lines = head.split(b"\r\n")
    status = int(lines[0].split(b" ")[1])
    headers = dict(line.split(b": ", 1) for line in lines[1:])
    assert int(headers[b"Content-Length"]) == len(body), raw[:512]
    assert headers[b"Connection"] == b"close"
    return status, headers, body


class HTTPTests(unittest.TestCase):
    def setUp(self):
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            self.port = probe.getsockname()[1]
        self.process = subprocess.Popen(
            [SERVER, str(self.port)], stdout=subprocess.PIPE, stderr=subprocess.PIPE
        )
        self.addCleanup(self.stop_server)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                self.fail(f"startup failed: {self.process.communicate()}")
            try:
                with self.connect():
                    return
            except OSError:
                time.sleep(0.02)
        self.fail("server startup timed out")

    def stop_server(self):
        if self.process.poll() is None:
            self.process.terminate()
        try:
            stdout, stderr = self.process.communicate(timeout=7)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.communicate()
            self.fail("shutdown timed out")
        self.assertEqual(self.process.returncode, 0, (stdout, stderr))
        self.assertNotIn(b"Sanitizer", stderr, stderr)
        self.assertNotIn(b"runtime error:", stderr, stderr)

    def connect(self):
        return socket.create_connection(("127.0.0.1", self.port), timeout=7)

    def exchange(self, raw):
        with self.connect() as client:
            client.sendall(raw)
            # 불완전한 본문 테스트도 서버가 EOF를 볼 수 있게 송신 방향만 닫는다.
            client.shutdown(socket.SHUT_WR)
            return read_response(client)

    def assert_alive(self):
        status, _, body = self.exchange(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
        self.assertEqual((status, body), (200, b"Hello"))

    def assert_no_response_yet(self, client):
        client.settimeout(0.1)
        with self.assertRaises(socket.timeout):
            client.recv(1)
        client.settimeout(7)

    def test_curl_get_and_post(self):
        base = f"http://127.0.0.1:{self.port}"
        for args, expected in [
            ([base + "/"], b"Hello"),
            ([base + "/hello?name=Jungle"], b"Hello, Jungle!\n"),
            (["--data-binary", "hello", base + "/echo"], b"hello"),
        ]:
            result = subprocess.run(
                ["curl", "--noproxy", "*", "-fsS", "--max-time", "5", *args],
                capture_output=True, timeout=7, check=True
            )
            self.assertEqual(result.stdout, expected)

    def test_dynamic_query(self):
        for target, expected in [
            (b"/hello", b"Hello, World!\n"),
            (b"/hello?name=A%20B+C", b"Hello, A B C!\n"),
            (b"/hello?name=%EC%A0%95%EA%B8%80", "Hello, 정글!\n".encode()),
        ]:
            status, _, body = self.exchange(b"GET " + target + b" HTTP/1.1\r\nHost: x\r\n\r\n")
            self.assertEqual((status, body), (200, expected))

    def test_fragmented_request_line_and_headers(self):
        with self.connect() as client:
            client.sendall(b"GE")
            self.assert_no_response_yet(client)
            client.sendall(b"T /hello?name=Split HTTP/1.1\r\nHost: x\r\n\r")
            self.assert_no_response_yet(client)
            client.sendall(b"\n")
            status, _, body = read_response(client)
            self.assertEqual((status, body), (200, b"Hello, Split!\n"))

    def test_fragmented_binary_body(self):
        payload = b"hel\x00\xff\r\n\r\nlo"
        header = b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: %d\r\n\r\n" % len(payload)
        with self.connect() as client:
            client.sendall(header + payload[:3])
            self.assert_no_response_yet(client)
            client.sendall(payload[3:-1])
            self.assert_no_response_yet(client)
            client.sendall(payload[-1:])
            status, headers, body = read_response(client)
            self.assertEqual((status, body), (200, payload))
            self.assertEqual(headers[b"Content-Type"], b"application/octet-stream")

    def test_coalesced_binary_body(self):
        payload = bytes(range(256)) * 16
        header = b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: %d\r\n\r\n" % len(payload)
        status, _, body = self.exchange(header + payload)
        self.assertEqual((status, body), (200, payload))

    def test_maximum_body_and_empty_body(self):
        for payload in (b"", bytes(range(256)) * (BODY_LIMIT // 256)):
            header = b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: %d\r\n\r\n" % len(payload)
            status, _, body = self.exchange(header + payload)
            self.assertEqual(status, 200)
            self.assertEqual(body, payload)

    def test_incomplete_body_and_headers(self):
        for raw in [
            b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhel",
            b"GET / HTTP/1.1\r\nHost: x\r\n",
        ]:
            self.assertEqual(self.exchange(raw)[0], 400)
        self.assert_alive()

    def test_body_timeout(self):
        with self.connect() as client:
            client.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhel")
            self.assertEqual(read_response(client)[0], 408)
        self.assert_alive()

    def test_header_timeout(self):
        with self.connect() as client:
            client.sendall(b"GET / HTTP/1.1\r\n")
            self.assertEqual(read_response(client)[0], 408)
        self.assert_alive()

    def test_errors_and_recovery(self):
        cases = [
            (b"GET /missing HTTP/1.1\r\nHost: x\r\n\r\n", 404),
            (b"DELETE / HTTP/1.1\r\nHost: x\r\n\r\n", 501),
            (b"GET / HTTP/1.1\r\n\r\n", 400),
            (b"POST /echo HTTP/1.1\r\nHost: x\r\n\r\n", 411),
            (b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 1048577\r\n\r\n", 413),
            (b"POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n", 501),
            (b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\n", 400),
            (b"POST /echo HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\nContent-Length: 1\r\n\r\n", 417),
            (b"GET /hello?name=%00 HTTP/1.1\r\nHost: x\r\n\r\n", 400),
        ]
        for raw, expected in cases:
            with self.subTest(raw=raw):
                self.assertEqual(self.exchange(raw)[0], expected)
                self.assert_alive()

    def test_method_not_allowed(self):
        for raw, allow in [
            (b"GET /echo HTTP/1.1\r\nHost: x\r\n\r\n", b"POST"),
            (b"POST /hello HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n", b"GET"),
        ]:
            status, headers, _ = self.exchange(raw)
            self.assertEqual(status, 405)
            self.assertEqual(headers[b"Allow"], allow)

    def test_header_and_target_limits(self):
        prefix = b"GET / HTTP/1.1\r\nHost: x\r\nX-Pad: "
        suffix = b"\r\n\r\n"
        exact = prefix + b"a" * (8192 - len(prefix) - len(suffix)) + suffix
        self.assertEqual(self.exchange(exact)[0], 200)
        self.assertEqual(self.exchange(b"a" * 8192)[0], 431)
        for length, status in [(2048, 404), (2049, 414)]:
            target = b"/" + b"a" * (length - 1)
            raw = b"GET " + target + b" HTTP/1.1\r\nHost: x\r\n\r\n"
            self.assertEqual(self.exchange(raw)[0], status)
        self.assert_alive()

    def test_http10_and_get_with_body(self):
        self.assertEqual(self.exchange(b"GET / HTTP/1.0\r\n\r\n")[0], 200)
        with self.connect() as client:
            client.sendall(b"GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\na")
            self.assert_no_response_yet(client)
            client.sendall(b"bc")
            self.assertEqual(read_response(client)[2], b"Hello")

    def test_repeated_connections_and_peer_reset(self):
        for _ in range(30):
            self.assert_alive()
        with self.connect() as client:
            client.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
            client.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
        self.assert_alive()

    def test_port_conflict_and_cli(self):
        for args in [[], ["0"], ["65536"], ["80abc"], ["8080", "extra"], [str(self.port)]]:
            result = subprocess.run([SERVER, *args], capture_output=True, timeout=5)
            self.assertNotEqual(result.returncode, 0)
            self.assertNotIn(b"Sanitizer", result.stderr)

    def test_sigint_while_reading_body(self):
        with self.connect() as client:
            client.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhe")
            self.assert_no_response_yet(client)
            self.process.send_signal(signal.SIGINT)
            self.assertEqual(self.process.wait(timeout=7), 0)

    def test_sigterm_while_reading_header(self):
        with self.connect() as client:
            client.sendall(b"GET / HTTP/1.1\r\n")
            self.assert_no_response_yet(client)
            self.process.terminate()
            self.assertEqual(self.process.wait(timeout=7), 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
