#!/usr/bin/env python3
"""개인 엣지 케이스 E2E 검증: 잘못된 요청, 큰 본문·긴 URI, 백엔드 무응답.

`docs/iteration-4/chan95821/iteration-4-tests.md` §8의 관측 결과를 재현한다.
실행: `python3 scripts/verify_edge_cases.py`
ASAN 빌드로 실행하려면 `SERVER_BIN=build/server_asan python3 ...`.
"""
import json
import os
import signal
import socket
import subprocess
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SERVER_BIN = Path(os.environ.get("SERVER_BIN", ROOT / "build/server"))
MOCK = ROOT / "tools/mock_backend.py"


def pick_free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait_for_port(port, timeout=5.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return
        except OSError:
            time.sleep(0.03)
    raise RuntimeError(f"no listener on {port}")


class Server:
    def __init__(self, config_text=None):
        self.port = pick_free_port()
        self.config = None
        args = [str(SERVER_BIN), str(self.port)]
        if config_text is not None:
            handle = tempfile.NamedTemporaryFile("w", suffix=".conf", delete=False)
            handle.write(config_text)
            handle.close()
            self.config = Path(handle.name)
            args.append(str(self.config))
        self.proc = subprocess.Popen(
            args, cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )
        wait_for_port(self.port)

    def stop(self):
        self.proc.send_signal(signal.SIGINT)
        try:
            self.proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        if self.config:
            self.config.unlink(missing_ok=True)

    def fd_count(self):
        try:
            return len(list(Path(f"/proc/{self.proc.pid}/fd").iterdir()))
        except OSError:
            return None


class MockBackend:
    def __init__(self, *extra):
        self.port = pick_free_port()
        self.proc = subprocess.Popen(
            ["python3", str(MOCK), "--port", str(self.port), "--id", "mock", *extra],
            cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        wait_for_port(self.port)

    def stop(self):
        self.proc.terminate()
        try:
            self.proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()


class RawBackend:
    """Accepts connections and runs one handler per connection."""

    def __init__(self, handler):
        self.port = pick_free_port()
        self.handler = handler
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", self.port))
        self.sock.listen(16)
        self.sock.settimeout(0.2)
        self.stop_flag = threading.Event()
        self.thread = threading.Thread(target=self._serve, daemon=True)
        self.thread.start()

    def _serve(self):
        while not self.stop_flag.is_set():
            try:
                conn, _ = self.sock.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            threading.Thread(target=self._wrap, args=(conn,), daemon=True).start()

    def _wrap(self, conn):
        try:
            self.handler(conn)
        except OSError:
            pass
        finally:
            try:
                conn.close()
            except OSError:
                pass

    def stop(self):
        self.stop_flag.set()
        self.sock.close()
        self.thread.join(timeout=1)


def read_response(sock, timeout=5.0, read_to_eof=True):
    sock.settimeout(timeout)
    data = bytearray()
    started = time.monotonic()
    while True:
        try:
            chunk = sock.recv(65536)
        except socket.timeout:
            break
        if not chunk:
            break
        data.extend(chunk)
        if not read_to_eof:
            break
    elapsed = time.monotonic() - started
    head, _, body = bytes(data).partition(b"\r\n\r\n")
    if not head:
        return None, {}, b"", elapsed, bytes(data)
    lines = head.split(b"\r\n")
    try:
        status = int(lines[0].split(b" ")[1])
    except (IndexError, ValueError):
        status = None
    headers = {}
    for line in lines[1:]:
        if b":" in line:
            name, value = line.split(b":", 1)
            headers[name.lower()] = value.strip()
    return status, headers, body, elapsed, bytes(data)


def exchange(port, payload, timeout=5.0, half_close=False, read_to_eof=True):
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as sock:
        sock.sendall(payload)
        if half_close:
            sock.shutdown(socket.SHUT_WR)
        return read_response(sock, timeout=timeout, read_to_eof=read_to_eof)


results = []


def check(name, condition, detail):
    results.append((name, bool(condition), detail))
    print(("PASS " if condition else "FAIL ") + name + " | " + detail, flush=True)


def main():
    # Server without proxy backends needed for malformed/large tests.
    server = Server()
    try:
        # --- malformed start line / headers ---
        cases = [
            ("missing Host", b"GET / HTTP/1.1\r\n\r\n", 400),
            ("invalid method char", b"G@T / HTTP/1.1\r\nHost: a\r\n\r\n", 400),
            ("lowercase method", b"get / HTTP/1.1\r\nHost: a\r\n\r\n", 501),
            ("empty method", b" / HTTP/1.1\r\nHost: a\r\n\r\n", 400),
            ("HTTP/1.0", b"GET / HTTP/1.0\r\nHost: a\r\n\r\n", 505),
            ("HTTP/2.0", b"GET / HTTP/2.0\r\nHost: a\r\n\r\n", 505),
            ("HTTP/1.x", b"GET / HTTP/1.x\r\nHost: a\r\n\r\n", 400),
            ("HTTP/1.1 trailing junk", b"GET / HTTP/1.1 junk\r\nHost: a\r\n\r\n", 400),
            ("header without colon", b"GET / HTTP/1.1\r\nHost: a\r\nBroken\r\n\r\n", 400),
            ("header name with space", b"GET / HTTP/1.1\r\nHost: a\r\nBad Name: x\r\n\r\n", 400),
            ("control char in header value",
             b"GET / HTTP/1.1\r\nHost: a\r\nX: bad\x01value\r\n\r\n", 400),
            ("space before colon", b"GET / HTTP/1.1\r\nHost : a\r\n\r\n", 400),
            ("HEAD dispatches as unsupported", b"HEAD / HTTP/1.1\r\nHost: a\r\n\r\n", 501),
            ("body bytes with no CL ignored", b"POST /echo HTTP/1.1\r\nHost: a\r\n\r\nabc", 200),
        ]
        for name, wire, expected in cases:
            status, _, body, elapsed, raw = exchange(server.port, wire)
            check(name, status == expected,
                  f"expected {expected}, got {status} ({elapsed*1000:.0f} ms)")

        # --- incomplete header ---
        status, _, _, elapsed, raw = exchange(
            server.port, b"GET / HTTP/1.1\r\nHost: a\r\n", half_close=True)
        check("incomplete header + EOF", status == 400,
              f"expected 400, got {status} ({elapsed*1000:.0f} ms)")

        with socket.create_connection(("127.0.0.1", server.port), timeout=1) as sock:
            sock.sendall(b"GET / HTTP/1.1\r\nHost: a\r\n")
            start = time.monotonic()
            status, _, _, _, _ = read_response(sock, timeout=10)
            elapsed = time.monotonic() - start
        check("incomplete header idle timeout (per-call 5 s)",
              4.5 <= elapsed <= 6.5 and status == 408,
              f"expected 408 after ~5 s, got {status} after {elapsed:.2f} s")

        with socket.create_connection(("127.0.0.1", server.port), timeout=1) as sock:
            sock.sendall(b"POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 10\r\n\r\nx")
            start = time.monotonic()
            status, _, _, _, _ = read_response(sock, timeout=10)
            elapsed = time.monotonic() - start
        check("incomplete body idle timeout (per-call 5 s)",
              4.5 <= elapsed <= 6.5 and status == 408,
              f"expected 408 after ~5 s, got {status} after {elapsed:.2f} s")

        # zero-byte connect then close, server must survive
        with socket.create_connection(("127.0.0.1", server.port), timeout=1):
            pass
        status, _, _, _, _ = exchange(
            server.port, b"GET / HTTP/1.1\r\nHost: a\r\n\r\n")
        check("server survives empty connection", status == 200, f"got {status}")

        # --- long URI ---
        long_path = b"/" + b"a" * 8001
        wire = b"GET " + long_path + b" HTTP/1.1\r\nHost: a\r\n\r\n"
        status, _, _, elapsed, _ = exchange(server.port, wire)
        check("long URI 8002 bytes -> 404", status == 404,
              f"header {len(wire)} B, got {status} ({elapsed*1000:.0f} ms)")

        long_query = b"/hello?" + b"q" * 7000
        wire = b"GET " + long_query + b" HTTP/1.1\r\nHost: a\r\n\r\n"
        status, _, body, elapsed, _ = exchange(server.port, wire)
        check("long query 7000 bytes -> 200 Hello",
              status == 200 and body == b"Hello",
              f"header {len(wire)} B, got {status} body={body[:10]!r}")

        giant_target = b"/" + b"a" * 8180
        wire = b"GET " + giant_target + b" HTTP/1.1\r\nHost: a\r\n\r\n"
        status, _, _, _, _ = exchange(server.port, wire)
        check("URI pushing header over 8192 -> 431", status == 431,
              f"header {len(wire)} B, got {status}")

        # --- body size ---
        payload = b"X" * 65536
        wire = (b"POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 65536\r\n\r\n" + payload)
        status, _, body, elapsed, _ = exchange(server.port, wire, timeout=10)
        check("exact 65536 body -> 200 echo",
              status == 200 and body == payload,
              f"got {status}, {len(body)} bytes ({elapsed*1000:.0f} ms)")

        wire = b"POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 65537\r\n\r\n"
        status, _, _, elapsed, _ = exchange(server.port, wire)
        check("declared 65537 body -> 413 before body read", status == 413,
              f"got {status} ({elapsed*1000:.0f} ms)")

        wire = b"POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 10485760\r\n\r\n"
        status, _, _, elapsed, _ = exchange(server.port, wire)
        check("declared 10 MiB body -> 413 before body read", status == 413,
              f"got {status} ({elapsed*1000:.0f} ms)")

        # Client keeps streaming 1 MiB after the early 413; server must answer,
        # stop reading, and stay healthy rather than OOM or hang.
        with socket.create_connection(("127.0.0.1", server.port), timeout=5) as sock:
            sock.sendall(b"POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 1048576\r\n\r\n")
            send_state = []

            def stream():
                try:
                    for _ in range(16):
                        sock.sendall(b"Z" * 65536)
                except OSError as error:
                    send_state.append(type(error).__name__)
                else:
                    send_state.append("sent")

            writer = threading.Thread(target=stream, daemon=True)
            writer.start()
            status, _, _, elapsed, _ = read_response(sock, timeout=5)
            writer.join(timeout=2)
        check("streaming 1 MiB into early 413 ends cleanly",
              status == 413 and not writer.is_alive(),
              f"status={status}, {elapsed*1000:.0f} ms, send={send_state}")

        fd_after = server.fd_count()
        status, _, _, _, _ = exchange(
            server.port, b"GET / HTTP/1.1\r\nHost: a\r\n\r\n")
        check("healthy after malformed/large cases",
              status == 200 and server.fd_count() == fd_after,
              f"status={status}, fd {fd_after} -> {server.fd_count()}")
    finally:
        server.stop()

    # --- proxy: normal backend accepts 64 KiB body ---
    submit = MockBackend()
    lookup = MockBackend()
    config = f"submit 127.0.0.1:{submit.port}\nlookup 127.0.0.1:{lookup.port}\n"
    server = Server(config)
    try:
        payload = b"B" * 65536
        wire = (b"POST /api/submissions HTTP/1.1\r\nHost: demo.example\r\n"
                b"Content-Length: 65536\r\n\r\n" + payload)
        status, headers, body, elapsed, _ = exchange(server.port, wire, timeout=10)
        try:
            received = json.loads(body).get("received")
        except ValueError:
            received = None
        check("proxy forwards 64 KiB body intact",
              status == 201 and received == 65536,
              f"status={status}, received={received}, {elapsed*1000:.0f} ms")

        wire = (b"POST /api/submissions HTTP/1.1\r\nHost: demo.example\r\n"
                b"Content-Length: 65537\r\n\r\n")
        status, _, _, elapsed, _ = exchange(server.port, wire)
        check("proxy route rejects 65537 body -> 413", status == 413,
              f"got {status} ({elapsed*1000:.0f} ms)")
    finally:
        server.stop()
        submit.stop()
        lookup.stop()

    # --- proxy: body progresses slower than the 1 s idle bound but never stalls ---
    slow = MockBackend("--body-bytes", "2000", "--slow-body-ms", "300", "--chunk-bytes", "250")
    lookup = MockBackend()
    config = f"submit 127.0.0.1:{slow.port}\nlookup 127.0.0.1:{lookup.port}\n"
    server = Server(config)
    try:
        wire = (b"POST /api/submissions HTTP/1.1\r\nHost: demo.example\r\n"
                b"Content-Length: 1\r\n\r\nx")
        status, headers, body, elapsed, _ = exchange(server.port, wire, timeout=10)
        check("slow but progressing backend body completes (>1 s total)",
              status == 201 and headers.get(b"content-length") == b"2000" and
              len(body) == 2000 and 1.5 <= elapsed <= 4.0,
              f"status={status}, {len(body)} B, {elapsed:.2f} s")
    finally:
        server.stop()
        slow.stop()
        lookup.stop()

    # --- proxy: backend responds slowly but within the 3 s header wait ---
    delayed = MockBackend("--delay-ms", "2000")
    lookup = MockBackend()
    config = f"submit 127.0.0.1:{delayed.port}\nlookup 127.0.0.1:{lookup.port}\n"
    server = Server(config)
    try:
        wire = (b"POST /api/submissions HTTP/1.1\r\nHost: demo.example\r\n"
                b"Content-Length: 1\r\n\r\nx")
        status, _, body, elapsed, _ = exchange(server.port, wire, timeout=6)
        check("backend responds after 2 s -> 201 within 3 s wait",
              status == 201 and 1.8 <= elapsed <= 3.8,
              f"status={status}, {elapsed:.2f} s, body={body[:40]!r}")
    finally:
        server.stop()
        delayed.stop()
        lookup.stop()

    # --- proxy: backend never responds ---
    hang = MockBackend("--hang-rate", "1", "--hang-ms", "10000")
    lookup = MockBackend()
    config = f"submit 127.0.0.1:{hang.port}\nlookup 127.0.0.1:{lookup.port}\n"
    server = Server(config)
    try:
        wire = (b"POST /api/submissions HTTP/1.1\r\nHost: demo.example\r\n"
                b"Content-Length: 1\r\n\r\nx")
        status, _, body, elapsed, _ = exchange(server.port, wire, timeout=6)
        check("backend silent -> 504 Gateway Timeout",
              status == 504 and 2.5 <= elapsed <= 4.5,
              f"status={status}, {elapsed:.2f} s, body={body[:40]!r}")

        status, _, body, elapsed, _ = exchange(
            server.port, b"GET /api/submission-status HTTP/1.1\r\nHost: demo.example\r\n\r\n")
        check("lookup backend still usable after submit timeout", status == 200,
              f"status={status}, {elapsed*1000:.0f} ms")
    finally:
        server.stop()
        hang.stop()
        lookup.stop()

    # --- proxy: backend sends partial status line then stalls ---
    def partial_header(conn):
        conn.recv(65536)
        conn.sendall(b"HTTP/1.1 200 OK\r\n")
        time.sleep(5)

    backend = RawBackend(partial_header)
    config = f"submit 127.0.0.1:{backend.port}\nlookup 127.0.0.1:{backend.port}\n"
    server = Server(config)
    try:
        wire = (b"POST /api/submissions HTTP/1.1\r\nHost: demo.example\r\n"
                b"Content-Length: 1\r\n\r\nx")
        status, _, body, elapsed, _ = exchange(server.port, wire, timeout=6)
        check("partial backend header -> 504 within total deadline",
              status == 504 and 2.5 <= elapsed <= 4.5,
              f"status={status}, {elapsed:.2f} s, body={body[:40]!r}")
    finally:
        server.stop()
        backend.stop()

    # --- proxy: backend body stalls after headers (client got 200 already) ---
    def partial_body(conn):
        conn.recv(65536)
        conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 100\r\n\r\nhello")
        time.sleep(5)

    backend = RawBackend(partial_body)
    config = f"submit 127.0.0.1:{backend.port}\nlookup 127.0.0.1:{backend.port}\n"
    server = Server(config)
    try:
        wire = (b"POST /api/submissions HTTP/1.1\r\nHost: demo.example\r\n"
                b"Content-Length: 1\r\n\r\nx")
        status, headers, body, elapsed, raw = exchange(server.port, wire, timeout=5)
        second_status = raw.count(b"HTTP/1.1 ")
        check("partial backend body closes without second response",
              status == 200 and headers.get(b"content-length") == b"100" and
              body == b"hello" and second_status == 1 and 0.8 <= elapsed <= 2.0,
              f"status={status}, body={body!r}, responses={second_status}, "
              f"elapsed={elapsed:.2f} s")
    finally:
        server.stop()
        backend.stop()

    failed = [name for name, ok, _ in results if not ok]
    print(f"\n{len(results) - len(failed)}/{len(results)} passed")
    if failed:
        print("FAILED: " + ", ".join(failed))
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
