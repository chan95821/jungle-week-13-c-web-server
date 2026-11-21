#!/usr/bin/env python3
import signal
import socket
import subprocess
import time

SERVER_BIN = "./server"
EXPECTED_HELLO = (
    b"HTTP/1.1 200 OK\r\n"
    b"Content-Type: text/plain\r\n"
    b"Content-Length: 5\r\n"
    b"Connection: close\r\n"
    b"\r\n"
    b"Hello"
)


def pick_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait_for_port(port: int, timeout: float = 3.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError(f"Server did not start on port {port}")


def start_server(port: int) -> subprocess.Popen:
    proc = subprocess.Popen(
        [SERVER_BIN, str(port)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    try:
        wait_for_port(port)
    except Exception:
        proc.kill()
        proc.wait()
        raise
    return proc


def stop_server(proc: subprocess.Popen) -> int:
    proc.send_signal(signal.SIGINT)
    return proc.wait(timeout=3.0)


def recv_all(sock: socket.socket) -> bytes:
    chunks = []
    while True:
        data = sock.recv(4096)
        if not data:
            break
        chunks.append(data)
    return b"".join(chunks)


def send_raw(port: int, payload: bytes) -> bytes:
    with socket.create_connection(("127.0.0.1", port), timeout=2.0) as s:
        s.sendall(payload)
        return recv_all(s)


def main() -> int:
    # 1. CLI 인자 오류 검증 (Iter 1 회귀)
    for args in [[], ["0"], ["65536"], ["80abc"], ["8080", "extra"]]:
        res = subprocess.run([SERVER_BIN, *args], capture_output=True, text=True)
        assert res.returncode != 0, f"Expected non-zero for args={args}"
    print("[PASS] CLI invalid argument checks")

    port = pick_free_port()
    proc = start_server(port)
    try:
        # 2. curl GET / 및 GET /hello, GET /hello?a=1
        for path in ["/", "/hello", "/hello?a=1"]:
            curl_res = subprocess.run(
                ["curl", "-sS", "-i", f"http://127.0.0.1:{port}{path}"],
                capture_output=True,
            )
            assert curl_res.returncode == 0, f"curl failed for {path}: {curl_res.stderr}"
            assert curl_res.stdout == EXPECTED_HELLO, f"Unexpected {path}: {curl_res.stdout!r}"
        print("[PASS] curl GET /, /hello, /hello?a=1")

        # 3. curl POST /echo
        curl_post = subprocess.run(
            [
                "curl", "-sS", "-i", "-X", "POST",
                "-H", "Content-Type: text/plain",
                "--data-binary", "ping pong",
                f"http://127.0.0.1:{port}/echo",
            ],
            capture_output=True,
        )
        assert curl_post.returncode == 0
        expected_echo = (
            b"HTTP/1.1 200 OK\r\n"
            b"Content-Type: text/plain\r\n"
            b"Content-Length: 9\r\n"
            b"Connection: close\r\n"
            b"\r\n"
            b"ping pong"
        )
        assert curl_post.stdout == expected_echo, f"Unexpected POST /echo: {curl_post.stdout!r}"
        print("[PASS] curl POST /echo")

        # 4. 연속 반복 연결 15회 (Iter 1 회귀)
        for i in range(15):
            resp = send_raw(port, b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n")
            assert resp == EXPECTED_HELLO, f"Iteration {i} mismatch: {resp!r}"
        print("[PASS] 15 sequential repeated requests")

        # 5. 헤더·본문 분할 전송 + 바이너리 NUL 포함 POST /echo
        binary_body = b"abc\x00def\xff123"
        with socket.create_connection(("127.0.0.1", port), timeout=2.0) as s:
            s.sendall(b"POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 11\r\n\r")
            time.sleep(0.03)
            s.sendall(b"\nabc\x00")
            time.sleep(0.03)
            s.sendall(b"def\xff123")
            resp = recv_all(s)
        expected_bin = (
            b"HTTP/1.1 200 OK\r\n"
            b"Content-Type: application/octet-stream\r\n"
            b"Content-Length: 11\r\n"
            b"Connection: close\r\n"
            b"\r\n" + binary_body
        )
        assert resp == expected_bin, f"Split binary echo mismatch: {resp!r}"
        print("[PASS] split packet & binary POST /echo")

        # 6. Connection: keep-alive 요청에도 Connection: close 응답
        resp = send_raw(
            port,
            b"GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n",
        )
        assert resp == EXPECTED_HELLO
        print("[PASS] Connection: close enforced")

        # 7. HTTP/1.0 Host 누락 허용 vs HTTP/1.1 Host 누락 400 거부
        resp_10 = send_raw(port, b"GET /hello HTTP/1.0\r\n\r\n")
        assert resp_10 == EXPECTED_HELLO, f"HTTP/1.0 mismatch: {resp_10!r}"
        resp_11_no_host = send_raw(port, b"GET /hello HTTP/1.1\r\n\r\n")
        assert resp_11_no_host.startswith(b"HTTP/1.1 400 Bad Request\r\n")
        print("[PASS] HTTP/1.0 vs HTTP/1.1 Host requirement")

        # 8. 상태 코드 오류 검증 (400, 404, 405, 411, 413, 414, 501, 505)
        cases = [
            (b"GET /hello HTTP/1.1\nHost: a\r\n\r\n", b"HTTP/1.1 400 Bad Request\r\n"),
            (b"GET /missing HTTP/1.1\r\nHost: a\r\n\r\n", b"HTTP/1.1 404 Not Found\r\n"),
            (b"GET /echo HTTP/1.1\r\nHost: a\r\n\r\n", b"HTTP/1.1 405 Method Not Allowed\r\n"),
            (b"PUT /echo HTTP/1.1\r\nHost: a\r\n\r\n", b"HTTP/1.1 405 Method Not Allowed\r\n"),
            (b"POST /echo HTTP/1.1\r\nHost: a\r\n\r\n", b"HTTP/1.1 411 Length Required\r\n"),
            (
                b"POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 1048577\r\n\r\n",
                b"HTTP/1.1 413 Payload Too Large\r\n",
            ),
            (
                b"GET /" + b"a" * 2048 + b" HTTP/1.1\r\nHost: a\r\n\r\n",
                b"HTTP/1.1 414 URI Too Long\r\n",
            ),
            (
                b"POST /echo HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n",
                b"HTTP/1.1 501 Not Implemented\r\n",
            ),
            (
                b"GET /hello HTTP/2.0\r\nHost: a\r\n\r\n",
                b"HTTP/1.1 505 HTTP Version Not Supported\r\n",
            ),
        ]
        for payload, prefix in cases:
            resp = send_raw(port, payload)
            assert resp.startswith(prefix), f"Expected {prefix!r}, got {resp!r}"
        print("[PASS] error status codes (400/404/405/411/413/414/501/505)")

        # 9. 조기 EOF(헤더/본문) & SIGPIPE 내성 & 8192B 헤더 상한 초과
        with socket.create_connection(("127.0.0.1", port), timeout=2.0) as s:
            s.sendall(b"GET /hello HTTP/1.1\r\n")
        with socket.create_connection(("127.0.0.1", port), timeout=2.0) as s:
            s.sendall(b"POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 100\r\n\r\npart")
        with socket.create_connection(("127.0.0.1", port), timeout=2.0) as s:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, b"\x01\x00\x00\x00\x00\x00\x00\x00")
            s.sendall(b"GET /hello HTTP/1.1\r\nHost: a\r\n\r\n")
        with socket.create_connection(("127.0.0.1", port), timeout=2.0) as s:
            s.sendall(b"A" * 9000)
            chunks = []
            try:
                while True:
                    chunk = s.recv(4096)
                    if not chunk:
                        break
                    chunks.append(chunk)
            except ConnectionResetError:
                pass  # 서버가 미수신 바이트 남긴 채 close() 시 후속 recv에서 TCP RST 발생 가능
            resp = b"".join(chunks)
            assert resp.startswith(b"HTTP/1.1 431 Request Header Fields Too Large\r\n"), (
                f"Expected 431 on oversized header, got {resp!r}"
            )
        assert send_raw(port, b"GET /hello HTTP/1.1\r\nHost: a\r\n\r\n") == EXPECTED_HELLO
        print("[PASS] early EOF, SIGPIPE, and 8192B header limit resilience")

        # 10. 포트 점유 실패 검증
        dup = subprocess.run([SERVER_BIN, str(port)], capture_output=True, text=True)
        assert dup.returncode != 0
        print("[PASS] duplicate port bind failure")
    finally:
        rc = stop_server(proc)
        assert rc == 0, f"Expected clean SIGINT exit 0, got {rc}"
        print("[PASS] clean SIGINT shutdown")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
