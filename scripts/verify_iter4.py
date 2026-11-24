#!/usr/bin/env python3
"""Iteration 4 기능 검증 후보. 부하·성능 측정 도구가 아니다.

실행 허락 후 저장소 루트에서 make verify-proxy로 실행한다.
테스트 fixture 3개와 C 서버를 직접 띄우며 외부 서비스에 요청하지 않는다.
"""
import contextlib
import http.client
import io
import os
from pathlib import Path
import signal
import socket
import socketserver
import subprocess
import threading
import time
from urllib.parse import parse_qs, urlsplit

from verify_iter1 import SERVER_BIN, pick_free_port, wait_for_port

ROOT = Path(__file__).resolve().parent.parent
BINARY = bytes(range(256)) * 65  # 내부 8KiB 버퍼를 넘고 NUL·0xff를 포함한다.


class Fixture(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, identity, role):
        super().__init__(("127.0.0.1", 0), Handler)
        self.identity, self.role = identity, role
        self.events = []
        self.lock = threading.Lock()


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        self.request.settimeout(5)
        try:
            self.exchange()
        except (OSError, ValueError):
            # client가 timeout 뒤 닫는 것은 장애 시나리오에서 의도한 동작이다.
            pass

    def exchange(self):
        line = self.rfile.readline(8193)
        if not line:
            return
        method, target, version = line.rstrip(b"\r\n").split(b" ")
        headers = {}
        while True:
            line = self.rfile.readline(8193)
            if line == b"\r\n":
                break
            if not line or len(line) > 8192:
                return
            name, value = line.split(b":", 1)
            headers[name.lower()] = value.strip()
        body = self.rfile.read(int(headers.get(b"content-length", b"0")))
        with self.server.lock:
            self.server.events.append((method, target, headers, body))
        parsed = urlsplit(target.decode("ascii"))
        expected = (b"POST", "/api/submissions") if self.server.role == "submission" else (b"GET", "/api/submission-status")
        if (method, parsed.path) != expected:
            self.request.sendall(b"HTTP/1.1 409 Wrong Backend\r\nContent-Length: 0\r\n\r\n")
            return
        case = parse_qs(parsed.query).get("case", ["normal"])[0]
        if case == "silent":
            time.sleep(3)
            return
        if case == "invalid":
            self.request.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nx")
            return
        if case == "interim":
            self.request.sendall(b"HTTP/1.1 103 Early Hints\r\nLink: </style.css>; rel=preload\r\n\r\n")
        payload = body if self.server.role == "submission" else BINARY
        status = b"201 Created" if self.server.role == "submission" else b"200 OK"
        if case in ("no-content", "not-modified"):
            status = b"204 No Content" if case == "no-content" else b"304 Not Modified"
            self.request.sendall(b"HTTP/1.1 " + status + b"\r\nX-Backend-Id: " + self.server.identity.encode() + b"\r\n\r\n")
            return
        prefix = (b"HTTP/1.1 " + status + b"\r\nContent-Type: application/octet-stream\r\n"
                  b"X-Backend-Id: " + self.server.identity.encode() + b"\r\n"
                  b"X-Target: " + target + b"\r\n"
                  b"Set-Cookie: a=1\r\nSet-Cookie: b=2\r\n"
                  b"X-Hop: secret\r\nConnection: X-Hop, close\r\n")
        if case in ("chunked", "bad-chunk", "truncated-chunk", "body-timeout"):
            self.request.sendall(prefix + b"Transfer-Encoding: chunked\r\n\r\n")
            if case == "bad-chunk":
                self.request.sendall(b"not-hex\r\n")
                return
            self.request.sendall(b"3;note=\"test\"\r\n" + payload[:3] + b"\r\n")
            if case == "truncated-chunk":
                return
            if case == "body-timeout":
                time.sleep(3)
                return
            self.request.sendall(f"{len(payload) - 3:x}\r\n".encode() + payload[3:] + b"\r\n0\r\nX-Trailer: done\r\n\r\n")
        elif case == "eof":
            self.request.sendall(prefix + b"\r\n" + payload)
        elif case == "truncated-length":
            self.request.sendall(prefix + b"Content-Length: 100\r\n\r\nshort")
        else:
            # 한 번의 send에 헤더와 본문 앞부분을 동반시키고 나머지는 분할.
            self.request.sendall(prefix + f"Content-Length: {len(payload)}\r\n\r\n".encode() + payload[:7])
            self.request.sendall(payload[7:])


@contextlib.contextmanager
def backend(identity, role):
    with Fixture(identity, role) as fixture:
        thread = threading.Thread(target=fixture.serve_forever, daemon=True)
        thread.start()
        try:
            yield fixture
        finally:
            fixture.shutdown()
            thread.join(timeout=5)


@contextlib.contextmanager
def proxy(endpoints):
    port = pick_free_port()
    with contextlib.ExitStack() as stack:
        # 임시 파일로 stderr를 받아 실패 반복 시 pipe 포화로 멈추지 않게 한다.
        import tempfile
        output = stack.enter_context(tempfile.TemporaryFile())
        proc = subprocess.Popen([SERVER_BIN, str(port), *endpoints], cwd=ROOT,
                                stdout=output, stderr=output)
        try:
            wait_for_port(port)
            yield proc, port
        finally:
            if proc.poll() is None:
                proc.send_signal(signal.SIGINT)
                try:
                    proc.wait(timeout=7)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
                    raise AssertionError("proxy did not stop after SIGINT")
            assert proc.returncode == 0, f"proxy exit={proc.returncode}"


def endpoint(fixture):
    return f"127.0.0.1:{fixture.server_address[1]}"


def raw_request(port, target, method=b"GET", body=b"", extra=b"", fragmented=False):
    head = (method + b" " + target + b" HTTP/1.1\r\nHost: original.example\r\n"
            + f"Content-Length: {len(body)}\r\n".encode() + extra + b"\r\n")
    with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
        sock.settimeout(5)
        if fragmented:
            sock.sendall(head[:-1])
            sock.sendall(head[-1:] + body[:1])
            sock.sendall(body[1:])
        else:
            sock.sendall(head + body)
        sock.shutdown(socket.SHUT_WR)
        data = bytearray()
        while True:
            part = sock.recv(8192)
            if not part:
                return bytes(data)
            data.extend(part)


class WireSocket:
    def __init__(self, wire):
        self.wire = wire

    def makefile(self, *args, **kwargs):
        return io.BytesIO(self.wire)


def parse(wire):
    response = http.client.HTTPResponse(WireSocket(wire))
    response.begin()
    return response


def fd_count(proc):
    directory = Path(f"/proc/{proc.pid}/fd")
    if not directory.is_dir():
        return None
    return len(list(directory.iterdir()))


def settled_fds(proc, expected):
    if expected is None:
        return
    # 연결 정리 완료를 기다리는 기능 판정용 기한. 성능 지표로 기록하지 않는다.
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        if fd_count(proc) == expected:
            return
        time.sleep(0.02)
    raise AssertionError(f"FD baseline={expected}, now={fd_count(proc)}")


def idle_baseline(proc):
    counts = []
    for _ in range(10):
        count = fd_count(proc)
        if count is None:
            return None
        counts.append(count)
        time.sleep(0.02)
    return min(counts)


def expect_continue(port):
    body = b"\x00continue\xff"
    with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
        sock.settimeout(5)
        sock.sendall(b"POST /api/submissions HTTP/1.1\r\nHost: original.example\r\n"
                     + f"Content-Length: {len(body)}\r\n".encode()
                     + b"Expect: 100-continue\r\n\r\n")
        interim = bytearray()
        while not interim.endswith(b"\r\n\r\n"):
            part = sock.recv(1)
            assert part, "EOF waiting for 100 Continue"
            interim.extend(part)
        assert interim == b"HTTP/1.1 100 Continue\r\n\r\n"
        sock.sendall(body)
        sock.shutdown(socket.SHUT_WR)
        wire = bytearray()
        while True:
            part = sock.recv(8192)
            if not part:
                break
            wire.extend(part)
        response = parse(bytes(wire))
        assert response.status == 201 and response.read() == body


def main():
    with backend("submission", "submission") as submission, backend("A", "status") as a, backend("B", "status") as b:
        endpoints = [endpoint(submission), endpoint(a), endpoint(b)]
        with proxy(endpoints) as (proc, port):
            baseline = idle_baseline(proc)
            for body in (b"", BINARY):
                target = b"/api/submissions?raw=%2F"
                response = parse(raw_request(port, target, b"POST", body,
                                            b"X-Private: hidden\r\nConnection: X-Private\r\n", True))
                assert response.status == 201
                assert response.read() == body
                assert response.getheader("X-Backend-Id") == "submission"
                assert response.getheader("X-Target") == target.decode()
                assert response.getheader("Content-Type") == "application/octet-stream"
                assert response.headers.get_all("Set-Cookie") == ["a=1", "b=2"]
                assert response.getheader("X-Hop") is None
                received = submission.events[-1]
                assert received[1] == target and received[3] == body
                assert received[2][b"host"] == b"original.example"
                assert b"x-private" not in received[2]
            assert not a.events and not b.events

            identities = set()
            for _ in range(6):  # 개인 기능 검증용 묶음; 팀 비교 요청 건수는 별도 합의.
                before = len(a.events) + len(b.events)
                response = parse(raw_request(port, b"/api/submission-status"))
                assert response.status == 200 and response.read() == BINARY
                identities.add(response.getheader("X-Backend-Id"))
                assert len(a.events) + len(b.events) == before + 1
            assert identities == {"A", "B"}
            assert len(submission.events) == 2

            expect_continue(port)
            assert b"expect" not in submission.events[-1][2]

            # 읽지 않고 즉시 닫는 클라이언트. 이후 정상 응답과 FD 회수를 확인한다.
            with socket.create_connection(("127.0.0.1", port), timeout=5) as abandoned:
                abandoned.sendall(b"GET /api/submission-status HTTP/1.1\r\nHost: local\r\n\r\n")

            for case in ("chunked", "eof", "interim", "no-content", "not-modified"):
                response = parse(raw_request(port, f"/api/submission-status?case={case}".encode()))
                expected = 204 if case == "no-content" else 304 if case == "not-modified" else 200
                assert response.status == expected
                assert response.read() == (b"" if expected in (204, 304) else BINARY)

            for case, expected in (("silent", 504), ("invalid", 502)):
                response = parse(raw_request(port, f"/api/submission-status?case={case}".encode()))
                assert response.status == expected
                response.read()

            for _ in range(3):
                for case in ("truncated-length", "truncated-chunk", "bad-chunk", "body-timeout"):
                    wire = raw_request(port, f"/api/submission-status?case={case}".encode())
                    assert wire.count(b"HTTP/1.1 ") == 1, "second error response appended"
                    response = parse(wire)
                    assert response.status == 200
                    try:
                        response.read()
                    except http.client.IncompleteRead:
                        pass
                    else:
                        raise AssertionError(f"{case}: client accepted incomplete response")
                response = parse(raw_request(port, b"/api/submission-status"))
                assert response.status == 200 and response.read() == BINARY
                settled_fds(proc, baseline)

            for target, method, expected in ((b"/api/submissions", b"GET", 405),
                                             (b"/api/submission-status", b"POST", 405),
                                             (b"/api/submission-status", b"DELETE", 501),
                                             (b"/api/submissions/extra", b"GET", 404)):
                response = parse(raw_request(port, target, method))
                assert response.status == expected
                response.read()
            # 프록시가 켜진 상태에서도 로컬 기존 경로가 동작해야 한다.
            response = parse(raw_request(port, b"/hello"))
            assert response.read() == b"Hello"
            response = parse(raw_request(port, b"/echo", b"POST", BINARY))
            assert response.read() == BINARY
            response = parse(raw_request(port, b"/greet?name=Alice"))
            assert response.status == 200 and b"Alice" in response.read()
            response = parse(raw_request(port, b"/cafe"))
            assert response.status == 200 and response.read() == (ROOT / "public/pages/cafe.html").read_bytes()
            settled_fds(proc, baseline)
            print("[PASS] routing, binary bodies, headers, framing, faults, recovery, local routes")
            if baseline is None:
                print("[UNVERIFIED] FD check requires Linux /proc")
            else:
                print("[PASS] FD count returns to baseline after repeated faults")

        # 포트를 바인딩하되 listen하지 않아 다른 프로세스가 점유하지 못하게 하고
        # 실제 TCP 연결 거절을 재현한다. 조회 A/B 모두 이 주소로 설정한다.
        with socket.socket() as refused:
            refused.bind(("127.0.0.1", 0))
            closed = f"127.0.0.1:{refused.getsockname()[1]}"
            with proxy([closed, closed, closed]) as (_, port):
                for _ in range(3):
                    response = parse(raw_request(port, b"/api/submission-status"))
                    assert response.status == 502
                    response.read()
                assert parse(raw_request(port, b"/hello")).read() == b"Hello"
        print("[PASS] connection refusal -> 502, SIGINT shutdown")

        # 실제 중계 대기 도중 SIGTERM: 신호가 poll을 깨우고 backend FD를 정리해야 한다.
        with proxy(endpoints) as (proc, port):
            with socket.create_connection(("127.0.0.1", port), timeout=5) as client:
                client.sendall(b"GET /api/submission-status?case=silent HTTP/1.1\r\nHost: local\r\n\r\n")
                deadline = time.monotonic() + 3
                while time.monotonic() < deadline:
                    with a.lock, b.lock:
                        waiting = any(event[1].endswith(b"case=silent") for event in a.events[-1:] + b.events[-1:])
                    if waiting:
                        break
                    time.sleep(0.01)
                else:
                    raise AssertionError("backend did not receive shutdown test request")
                proc.send_signal(signal.SIGTERM)
                assert proc.wait(timeout=3) == 0
        print("[PASS] SIGTERM during backend wait")


if __name__ == "__main__":
    main()
