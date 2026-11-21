#!/usr/bin/env python3
import signal
import socket
import subprocess
import sys
import time

SERVER_BIN = "./server"
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
    return proc.wait(timeout=3.0)


def read_all(sock: socket.socket) -> bytes:
    # Iteration 2는 헤더와 본문을 따로 send한다. TCP에는 호출 경계가 없으므로
    # 한 번의 recv로 응답 전체를 받는다는 기존 가정을 제거한다.
    chunks = []
    while True:
        chunk = sock.recv(4096)
        if not chunk:
            return b"".join(chunks)
        chunks.append(chunk)


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
            ["curl", "--noproxy", "*", "--max-time", "5", "-sS", "-i", f"http://127.0.0.1:{port}/"],
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
            data = read_all(s)
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
            assert read_all(s) == EXPECTED_RESP
        print("[PASS] early EOF & SIGPIPE resilience")

        # 6. 8192B 상한 초과 요청 거부 후 서버 생존 확인
        with socket.create_connection(("127.0.0.1", port), timeout=2.0) as s:
            s.sendall(b"A" * 8192)
            try:
                resp = read_all(s)
                # 파싱을 추가한 단계부터는 무응답 종료 대신 명시적인 431을 보낸다.
                assert resp.startswith(b"HTTP/1.1 431 "), resp
            except ConnectionResetError:
                pass  # 서버가 미수신 바이트 남긴 채 close() 시 TCP RST 정상 발생
        with socket.create_connection(("127.0.0.1", port), timeout=2.0) as s:
            s.sendall(b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n")
            assert read_all(s) == EXPECTED_RESP
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
            assert read_all(s) == EXPECTED_RESP
    finally:
        assert stop_server(proc2) == 0
    print("[PASS] SO_REUSEADDR immediate restart")

    # 9. 컨테이너는 비루트 사용자도 80번에 bind할 수 있다.
    # UID만으로 실패를 가정하면 서버를 정상 실행한 채 subprocess.run이 멈춘다.
    # 실제 bind 권한을 먼저 확인하고 거부되는 환경에서만 실패 동작을 검증한다.
    permission_denied = False
    with socket.socket() as probe:
        try:
            probe.bind(("0.0.0.0", 80))
        except PermissionError:
            permission_denied = True
        except OSError:
            pass  # 다른 프로세스의 점유는 권한 검증과 별개다.
    if permission_denied:
        priv = subprocess.run([SERVER_BIN, "80"], capture_output=True, text=True, timeout=3)
        assert priv.returncode != 0, "Expected bind failure on privileged port 80"
        print("[PASS] privileged port 80 bind failure")
    else:
        print("[SKIP] privileged-port permission check (bind is permitted or port occupied)")

    print("All Iteration 1 E2E verification checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
