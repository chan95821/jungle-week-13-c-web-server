#!/usr/bin/env python3
import os
import signal
import socket
import subprocess
import sys
import time

SERVER_BIN = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "build", "server")
EXPECTED_RESP = (
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
    try:
        return proc.wait(timeout=3.0)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
        raise


def receive_to_eof(sock):
    data = bytearray()
    while True:
        chunk = sock.recv(4096)
        if not chunk:
            return bytes(data)
        data.extend(chunk)


def main() -> int:
    # 1. CLI 인자 오류 검증
    for args in [[], ["0"], ["65536"], ["80abc"], ["8080", "extra"]]:
        res = subprocess.run([SERVER_BIN, *args], capture_output=True, text=True)
        assert res.returncode != 0, f"Expected non-zero for args={args}"
    print("[PASS] CLI invalid argument checks")

    port = pick_free_port()
    proc = start_server(port)
    try:
        # 2. curl -i 검증 (DoD)
        curl_res = subprocess.run(
            ["curl", "--noproxy", "*", "-sS", "-i", f"http://127.0.0.1:{port}/"],
            capture_output=True,
        )
        assert curl_res.returncode == 0, f"curl failed: {curl_res.stderr}"
        assert curl_res.stdout == EXPECTED_RESP, f"Unexpected curl output: {curl_res.stdout!r}"
        print("[PASS] curl -i 200 OK & exact response")

        # 3. 연속 반복 연결 (15회)
        for i in range(15):
            with socket.create_connection(("127.0.0.1", port), timeout=2.0) as s:
                s.sendall(b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n")
                data = b""
                while True:
                    chunk = s.recv(4096)
                    if not chunk:
                        break
                    data += chunk
                assert data == EXPECTED_RESP, f"Iteration {i} mismatch: {data!r}"
        print("[PASS] 15 sequential repeated requests")

        # 4. 패킷 분할 전송 (\r\n\r 과 \n 경계 분할)
        with socket.create_connection(("127.0.0.1", port), timeout=2.0) as s:
            s.sendall(b"GET / HTTP/1.1\r\nHost: localhost\r\n\r")
            time.sleep(0.05)
            s.sendall(b"\n")
            data = receive_to_eof(s)
            assert data == EXPECTED_RESP, f"Split request mismatch: {data!r}"
        print("[PASS] split packet request")

        # 5. 조기 종료(EOF) 및 응답 전/중 끊기(SIGPIPE 내성)
        with socket.create_connection(("127.0.0.1", port), timeout=2.0) as s:
            s.sendall(b"GET / HTTP/1.1\r\n")
        with socket.create_connection(("127.0.0.1", port), timeout=2.0) as s:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, b"\x01\x00\x00\x00\x00\x00\x00\x00")
            s.sendall(b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n")
        # 직후 정상 요청으로 서버 생존 확인
        with socket.create_connection(("127.0.0.1", port), timeout=2.0) as s:
            s.sendall(b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n")
            assert receive_to_eof(s) == EXPECTED_RESP
        print("[PASS] early EOF & SIGPIPE resilience")

        # 6. 8192B 상한 초과 요청 거부 후 서버 생존 확인
        with socket.create_connection(("127.0.0.1", port), timeout=2.0) as s:
            s.sendall(b"A" * 9000)
            resp = receive_to_eof(s)
            header, body = resp.split(b"\r\n\r\n", 1)
            assert header.startswith(b"HTTP/1.1 431 "), resp
            lengths = [line for line in header.split(b"\r\n") if line.lower().startswith(b"content-length:")]
            assert len(lengths) == 1 and len(body) == int(lengths[0].split(b":", 1)[1])
        with socket.create_connection(("127.0.0.1", port), timeout=2.0) as s:
            s.sendall(b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n")
            assert receive_to_eof(s) == EXPECTED_RESP
        print("[PASS] oversized header rejection & recovery")

        # 7. 이미 사용 중인 포트 bind 실패 확인
        dup = subprocess.run([SERVER_BIN, str(port)], capture_output=True, text=True)
        assert dup.returncode != 0, "Expected bind failure on already-used port"
        print("[PASS] duplicate port bind failure")
    finally:
        rc = stop_server(proc)
        assert rc == 0, f"Expected clean SIGINT exit code 0, got {rc}"
    print("[PASS] SIGINT clean shutdown")

    # 8. SO_REUSEADDR 즉시 재실행 확인
    proc2 = start_server(port)
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=2.0) as s:
            s.sendall(b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n")
            assert receive_to_eof(s) == EXPECTED_RESP
    finally:
        assert stop_server(proc2) == 0
    print("[PASS] SO_REUSEADDR immediate restart")

    # 9. 비루트 환경 특권 포트(80) bind 실패 확인
    with open("/proc/sys/net/ipv4/ip_unprivileged_port_start", encoding="ascii") as settings:
        privileged = 80 < int(settings.read())
    with open("/proc/self/status", encoding="ascii") as status:
        capabilities = next(int(line.split()[1], 16) for line in status if line.startswith("CapEff:"))
    if os.geteuid() != 0 and privileged and not (capabilities & (1 << 10)):
        priv = subprocess.run([SERVER_BIN, "80"], capture_output=True, text=True)
        assert priv.returncode != 0, "Expected bind failure on privileged port 80"
        print("[PASS] privileged port 80 bind failure")

    print("All Iteration 1 E2E verification checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
