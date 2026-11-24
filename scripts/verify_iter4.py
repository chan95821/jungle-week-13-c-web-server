#!/usr/bin/env python3
"""Iteration 4 reverse proxy, path routing, lookup balancing, and config-file checks.

백엔드 포트는 매 실행 빈 포트를 잡고, 서버에는 임시 라우팅 설정 파일을 넘긴다.
따라서 개발 중인 다른 백엔드(8001~8003)와 포트가 겹치지 않는다.
"""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import time

from verify_iter1 import SERVER_BIN, pick_free_port, stop_server, wait_for_port

REPO_ROOT = Path(__file__).resolve().parent.parent
EXCLUSION_WAIT_SECONDS = 2.5


def make_handler(name, headers=(), binary_path=None, binary_bytes=32):
    class BackendHandler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"
        server_name = name

        def do_GET(self):
            path = self.path.split("?", 1)[0]
            if binary_path and path == binary_path:
                body = bytes(i % 256 for i in range(binary_bytes))
            else:
                body = ('{"server":"%s","path":"%s"}' % (self.server_name, self.path)).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            for line in headers:
                self.send_header(*line.split(": ", 1))
            self.end_headers()
            self.wfile.write(body)

        def do_POST(self):
            length = int(self.headers.get("Content-Length", "0"))
            received = self.rfile.read(length)
            body = ('{"server":"%s","received":%d}' % (self.server_name, len(received))).encode()
            self.send_response(201)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            for line in headers:
                self.send_header(*line.split(": ", 1))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *args):
            pass

    return BackendHandler


class Backend:
    """같은 고정 상태를 돌려주는 백엔드. A·B는 식별자만 다르다."""

    def __init__(self, port, name, headers=(), binary_path=None, binary_bytes=32):
        try:
            self.server = ThreadingHTTPServer(
                ("127.0.0.1", port),
                make_handler(name, headers, binary_path, binary_bytes))
        except OSError as error:
            raise SystemExit(f"backend port {port} unavailable: {error}") from error
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.running = False

    def start(self):
        self.running = True
        self.thread.start()

    def stop(self):
        if not self.running:
            return
        self.running = False
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)


def write_routes_config(submit_port, lookup_a_port, lookup_b_port):
    handle = tempfile.NamedTemporaryFile("w", suffix=".conf", delete=False)
    handle.write("# Iteration 4 E2E 검증용 임시 라우팅 설정\n")
    handle.write(f"submit 127.0.0.1:{submit_port}\n")
    handle.write(f"lookup 127.0.0.1:{lookup_a_port}\n")
    handle.write(f"lookup 127.0.0.1:{lookup_b_port}\n")
    handle.close()
    return Path(handle.name)


def start_configured_server(port, config_path):
    proc = subprocess.Popen(
        [SERVER_BIN, str(port), str(config_path)],
        cwd=REPO_ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    try:
        wait_for_port(port)
    except Exception:
        proc.kill()
        proc.wait()
        raise
    return proc


def request(port, method, target, body=b""):
    """Connection: close 응답을 EOF까지 읽어 상태·헤더·본문을 돌려준다."""
    head = (
        f"{method} {target} HTTP/1.1\r\nHost: demo.example\r\n"
        f"Content-Length: {len(body)}\r\n\r\n"
    ).encode()
    with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
        sock.sendall(head + body)
        data = bytearray()
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            data.extend(chunk)

    raw_head, _, payload = bytes(data).partition(b"\r\n\r\n")
    lines = raw_head.split(b"\r\n")
    status = int(lines[0].split(b" ")[1])
    headers = {}
    for line in lines[1:]:
        name, value = line.split(b":", 1)
        headers[name.lower()] = value.strip()
    return status, headers, payload


def served_by(payload):
    return payload.decode().split('"server":"', 1)[1].split('"', 1)[0]


def alternates(servers):
    """정확한 분배 비율은 요구하지 않지만, 연속 조회는 두 서버를 번갈아 사용해야 한다."""
    return (set(servers) == {"lookup-a", "lookup-b"} and
            all(prev != cur for prev, cur in zip(servers, servers[1:])))


def fd_count(pid):
    try:
        return len(list(Path(f"/proc/{pid}/fd").iterdir()))
    except OSError:
        return None


def check_config_rejections():
    """설정 파일 오류는 서버 시작 전에 실패해야 한다."""
    port = pick_free_port()
    missing = REPO_ROOT / "does-not-exist-routes.conf"
    result = subprocess.run([SERVER_BIN, str(port), str(missing)],
                            cwd=REPO_ROOT, capture_output=True, text=True)
    assert result.returncode != 0, "missing config must fail before serving"

    handle = tempfile.NamedTemporaryFile("w", suffix=".conf", delete=False)
    handle.write("submit 127.0.0.1:8001\nmanagement 127.0.0.1:9000\n")
    handle.close()
    try:
        result = subprocess.run([SERVER_BIN, str(port), handle.name],
                                cwd=REPO_ROOT, capture_output=True, text=True)
        assert result.returncode != 0, "unknown key must fail before serving"
        assert "unknown key" in result.stderr, result.stderr
    finally:
        Path(handle.name).unlink()
    print("[PASS] invalid routes config fails before serving")


def check_relay_and_routing(port, binary_length=32):
    status, headers, payload = request(port, "POST", "/api/submissions?course=os", b"0123456789abcdef")
    assert status == 201, status
    assert headers[b"content-type"] == b"application/json", headers
    assert served_by(payload) == "submissions", payload
    assert b'"received":16' in payload, payload
    # 백엔드가 붙인 일반 헤더도 클라이언트까지 전달되어야 한다.
    assert headers.get(b"x-mock") == b"submissions", headers
    assert headers.get(b"cache-control") == b"no-store", headers

    # /api/hello는 0..255 반복 이진 본문으로 응답해 바이트 무결성을 확인한다.
    status, headers, payload = request(port, "GET", "/api/hello")
    assert status == 200, status
    assert payload == bytes(i % 256 for i in range(binary_length)), payload[:16]
    assert headers.get(b"x-mock") == b"submissions", headers

    servers = [served_by(request(port, "GET", "/api/submission-status")[2]) for _ in range(4)]
    assert alternates(servers), servers
    print("[PASS] routes, header forwarding, binary body, and round robin")


def check_rejections_and_regression(port):
    status, headers, _ = request(port, "GET", "/api/submissions")
    assert status == 405 and headers.get(b"allow") == b"POST", (status, headers)

    status, _, _ = request(port, "GET", "/api/submissions/extra")
    assert status == 404, status

    status, _, payload = request(port, "GET", "/hello")
    assert status == 200 and payload == b"Hello", payload
    status, _, _ = request(port, "GET", "/cafe")
    assert status == 200, status
    print("[PASS] method/path rejection and existing demo regression")


def check_backend_outage_and_recovery(port, lookup_b_port, lookup_b):
    lookup_b.stop()
    for _ in range(4):
        status, _, payload = request(port, "GET", "/api/submission-status")
        assert status == 200 and served_by(payload) == "lookup-a", (status, payload)

    lookup_b = Backend(lookup_b_port, "lookup-b")
    lookup_b.start()
    time.sleep(EXCLUSION_WAIT_SECONDS)
    servers = [served_by(request(port, "GET", "/api/submission-status")[2]) for _ in range(4)]
    assert alternates(servers), servers
    print("[PASS] outage fallback, temporary exclusion, and request-based recovery")
    return lookup_b


def check_all_lookup_down(port, lookup_a, lookup_b):
    lookup_a.stop()
    lookup_b.stop()
    assert request(port, "GET", "/api/submission-status")[0] == 502
    status, _, payload = request(port, "POST", "/api/submissions", b"x")
    assert status == 201 and served_by(payload) == "submissions"
    print("[PASS] both lookup backends down: lookup 502 while submission still relayed")


def check_fd_stability(port, server_pid):
    """A·B가 모두 내려간 상태에서 실패 요청을 반복해도 FD가 누적되지 않는지 확인한다."""
    before = fd_count(server_pid)
    if before is None:
        print("[SKIP] FD accounting needs /proc")
        return
    for _ in range(20):
        request(port, "GET", "/api/submission-status")
    after = fd_count(server_pid)
    assert before == after, (before, after)
    print(f"[PASS] repeated backend failures keep FD count stable at {after}")


def main():
    check_config_rejections()

    submit_port = pick_free_port()
    lookup_a_port = pick_free_port()
    lookup_b_port = pick_free_port()
    submission = Backend(
        submit_port, "submissions",
        headers=("X-Mock: submissions", "Cache-Control: no-store"),
        binary_path="/api/hello")
    lookup_a = Backend(lookup_a_port, "lookup-a")
    lookup_b = Backend(lookup_b_port, "lookup-b")
    for backend in (submission, lookup_a, lookup_b):
        backend.start()

    config_path = write_routes_config(submit_port, lookup_a_port, lookup_b_port)
    port = pick_free_port()
    proc = start_configured_server(port, config_path)
    try:
        check_relay_and_routing(port)
        check_rejections_and_regression(port)
        lookup_b = check_backend_outage_and_recovery(port, lookup_b_port, lookup_b)
        check_all_lookup_down(port, lookup_a, lookup_b)
        check_fd_stability(port, proc.pid)
    finally:
        stop_server(proc)
        config_path.unlink()
        for backend in (submission, lookup_a, lookup_b):
            backend.stop()
    print("All Iteration 4 reverse proxy checks passed.")


if __name__ == "__main__":
    main()
