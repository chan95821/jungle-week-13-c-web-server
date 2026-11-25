#!/usr/bin/env python3
"""Functional reverse-proxy verification with C backends, not a load benchmark."""
from contextlib import ExitStack
import http.client
import io
from pathlib import Path
import shutil
import signal
import socket
import struct
import subprocess
import tempfile
import time

from verify_iter1 import pick_free_port, wait_for_port, receive_to_eof

ROOT = Path(__file__).resolve().parent.parent
SERVER = ROOT / "build/server"
BACKEND = ROOT / "build/test_backend"


def stop_child(process):
    if process.poll() is None:
        process.send_signal(signal.SIGTERM)
        try:
            process.wait(timeout=6)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            raise AssertionError("child did not stop after SIGTERM")
    if process.stderr:
        process.stderr.close()


class WireSocket:
    def __init__(self, wire):
        self.wire = wire

    def makefile(self, *_args, **_kwargs):
        return io.BytesIO(self.wire)


def parse_response(wire, method="GET"):
    while wire.startswith(b"HTTP/1.1 "):
        end = wire.find(b"\r\n\r\n")
        if end < 0:
            break
        status = int(wire.split(b" ", 2)[1])
        if status >= 200:
            break
        wire = wire[end + 4:]
    response = http.client.HTTPResponse(WireSocket(wire), method=method)
    response.begin()
    body = response.read()
    return response.status, response.getheaders(), body


def header_values(headers, name):
    return [value for key, value in headers if key.lower() == name.lower()]


def request_wire(port, target=b"/api/submission-status", method=b"GET", body=b"",
                 headers=b"", timeout=8, fragments=False, include_content_length=True):
    length_header = (b"Content-Length: " + str(len(body)).encode() + b"\r\n"
                     if include_content_length else b"")
    head = (method + b" " + target + b" HTTP/1.1\r\nHost: original.example\r\n"
            + length_header + headers + b"Connection: close\r\n\r\n")
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as client:
        client.settimeout(timeout)
        if fragments:
            client.sendall(head[:9])
            client.sendall(head[9:] + body[:3])
            client.sendall(body[3:])
        else:
            client.sendall(head + body)
        client.shutdown(socket.SHUT_WR)
        return receive_to_eof(client)


class Fixture:
    def __init__(self, stack, directory):
        self.stack = stack
        self.directory = directory
        self.ports = {}
        while len(set(self.ports.values())) != 3 or len(self.ports) != 3:
            self.ports = {role: pick_free_port()
                          for role in ("submission", "query-a", "query-b")}
        self.backends = {}
        for role in self.ports:
            self.restart_backend(role)
        self.proxy_port = pick_free_port()
        while self.proxy_port in self.ports.values():
            self.proxy_port = pick_free_port()
        self.config = directory / "proxy.conf"
        self.config.write_text("".join(f"{role} 127.0.0.1:{port}\n"
                                       for role, port in self.ports.items()), encoding="ascii")
        self.proxy = self.start([str(SERVER), str(self.proxy_port), str(self.config)],
                                self.proxy_port)

    def start(self, argv, port):
        process = subprocess.Popen(argv, cwd=self.directory,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        self.stack.callback(stop_child, process)
        wait_for_port(port)
        if process.poll() is not None:
            raise AssertionError("child exited during startup")
        return process

    def restart_backend(self, role, mode="normal"):
        previous = self.backends.get(role)
        if previous is not None:
            stop_child(previous)
        self.backends[role] = self.start(
            [str(BACKEND), str(self.ports[role]), role, mode], self.ports[role])

    def query_mode(self, mode):
        self.restart_backend("query-a", mode)
        self.restart_backend("query-b", mode)

    def healthy_query(self):
        status, headers, body = parse_response(request_wire(self.proxy_port))
        assert status == 200 and body == b"status\0ready\xff"
        assert header_values(headers, "X-Backend-ID")[0] in ("query-a", "query-b")
        assert self.proxy.poll() is None

    def fd_count(self):
        directory = Path(f"/proc/{self.proxy.pid}/fd")
        return len(list(directory.iterdir())) if directory.is_dir() else None


def reject_protected_header(port, method, target, connection_lines):
    # Send headers only and leave the write side open until the final rejection.
    # A broken implementation that sends 100 or waits for the body must fail.
    with socket.create_connection(("127.0.0.1", port), timeout=3) as client:
        client.sendall(
            method + b" " + target + b" HTTP/1.1\r\nHost: original.example\r\n"
            b"Content-Length: 10\r\nExpect: 100-continue\r\n"
            + connection_lines + b"\r\n")
        wire = bytearray()
        while b"\r\n\r\n" not in wire:
            chunk = client.recv(4096)
            assert chunk, "EOF before rejection headers"
            wire.extend(chunk)
        assert wire.startswith(b"HTTP/1.1 400 "), bytes(wire)
        client.shutdown(socket.SHUT_WR)
        wire.extend(receive_to_eof(client))
        assert b"HTTP/1.1 100 " not in wire
        assert parse_response(bytes(wire))[0] == 400


def verify_routing_error_priority(port):
    # No body: routing/framing errors must win before 100 Continue or body reads.
    cases = [
        (b"GET", b"/api/submissions", 405, "POST", b"Content-Length: 10\r\n"),
        (b"POST", b"/api/submission-status", 405, "GET", b"Content-Length: 10\r\n"),
        (b"POST", b"/api/hello", 405, "GET", b"Content-Length: 10\r\n"),
        (b"POST", b"/hello", 405, "GET", b"Content-Length: 10\r\n"),
        (b"HEAD", b"/api/hello", 501, None, b"Content-Length: 10\r\n"),
        (b"PUT", b"/api/submissions", 501, None, b"Content-Length: 10\r\n"),
        (b"GET", b"/api/hello/", 404, None, b"Content-Length: 10\r\n"),
        (b"POST", b"/api/submissions-extra", 404, None, b"Content-Length: 10\r\n"),
        (b"HEAD", b"/missing", 501, None, b"Content-Length: 10\r\n"),
        (b"GET", b"/api/submissions", 413, None, b"Content-Length: 65537\r\n"),
        (b"GET", b"/api/submissions", 400, None, b"Content-Length: invalid\r\n"),
    ]
    for method, target, expected, allow, length_header in cases:
        with socket.create_connection(("127.0.0.1", port), timeout=4) as client:
            client.sendall(method + b" " + target + b" HTTP/1.1\r\nHost: test\r\n" +
                           length_header + b"Expect: 100-continue\r\nConnection: Host\r\n\r\n")
            client.shutdown(socket.SHUT_WR)
            wire = receive_to_eof(client)
        assert wire.startswith(f"HTTP/1.1 {expected} ".encode("ascii")), wire
        assert b"HTTP/1.1 100 " not in wire
        status, headers, _ = parse_response(wire, method.decode("ascii"))
        assert status == expected
        assert header_values(headers, "Allow") == ([] if allow is None else [allow])


def verify_protected_connection_options(fixture):
    cases = [
        (method, target, b"Connection: " + name + b"\r\n")
        for method, target in (
            (b"POST", b"/api/submissions"),
            (b"GET", b"/api/submission-status"),
            (b"GET", b"/api/hello"))
        for name in (b"Host", b"Content-Length", b"Transfer-Encoding")
    ]
    cases += [
        (b"POST", b"/api/submissions",
         b"cOnNeCtIoN:\tX-Hop, HoSt \t\r\n"),
        (b"POST", b"/api/submissions",
         b"Connection: X-Hop\r\nConnection: close, TRANSFER-ENCODING\r\n"),
    ]
    for method, target, lines in cases:
        reject_protected_header(fixture.proxy_port, method, target, lines)
    status, _, body = parse_response(request_wire(
        fixture.proxy_port, b"/hello", headers=b"Connection: Host\r\n"))
    assert status == 200 and body == b"Hello"

    # Passive connection detectors only: these sockets never serve HTTP.
    with ExitStack() as sockets:
        traps = []
        for _ in range(3):
            trap = sockets.enter_context(socket.socket())
            trap.bind(("127.0.0.1", 0))
            trap.listen()
            trap.setblocking(False)
            traps.append(trap)
        config = fixture.directory / "protected-fields.conf"
        config.write_text("".join(
            f"{role} 127.0.0.1:{trap.getsockname()[1]}\n"
            for role, trap in zip(("submission", "query-a", "query-b"), traps)),
            encoding="ascii")
        proxy_port = pick_free_port()
        proxy = fixture.start([str(SERVER), str(proxy_port), str(config)], proxy_port)
        try:
            verify_routing_error_priority(proxy_port)
            for method, target, lines in cases:
                reject_protected_header(proxy_port, method, target, lines)
            for trap in traps:
                try:
                    connection, _ = trap.accept()
                except BlockingIOError:
                    continue
                connection.close()
                raise AssertionError("rejected request reached a backend")
            assert proxy.poll() is None
        finally:
            stop_child(proxy)
    print("[PASS] routing error priority and protected Connection -> early rejection without backend; local routes intact")


def verify_request_length_headers(port):
    cases = [
        (b"GET", b"/api/submission-status", b"", False, "absent"),
        (b"GET", b"/api/submission-status", b"", True, "0"),
        (b"GET", b"/api/submission-status", b"a\0b", True, "3"),
        (b"POST", b"/api/submissions", b"", False, "0"),
        (b"POST", b"/api/submissions", b"", True, "0"),
        (b"POST", b"/api/submissions", b"a\0b", True, "3"),
    ]
    for method, target, payload, include_length, expected in cases:
        status, headers, body = parse_response(request_wire(
            port, target, method, payload, include_content_length=include_length),
            method.decode("ascii"))
        assert status == (201 if method == b"POST" else 200)
        assert header_values(headers, "X-Received-Content-Length") == [expected]
        assert body == (payload if method == b"POST" else b"status\0ready\xff")
    print("[PASS] GET length omission, explicit lengths and empty/binary POST framing")


def main():
    with tempfile.TemporaryDirectory(prefix="verify-iter4-") as temporary, ExitStack() as stack:
        directory = Path(temporary)
        shutil.copytree(ROOT / "public", directory / "public")
        fixture = Fixture(stack, directory)
        port = fixture.proxy_port
        verify_protected_connection_options(fixture)

        status, headers, body = parse_response(request_wire(port, b"/api/hello"))
        assert status == 200 and body == b"Hello"
        assert header_values(headers, "X-Backend-ID") == ["query-a"]
        observed = []
        for _ in range(4):
            status, headers, body = parse_response(request_wire(port))
            assert status == 200 and body == b"status\0ready\xff"
            observed += header_values(headers, "X-Backend-ID")
        assert observed == ["query-b", "query-a", "query-b", "query-a"]
        print("[PASS] #4 hello and #17 query A/B shared round-robin")
        verify_request_length_headers(port)

        payload = bytes(range(256)) * 256
        status, headers, body = parse_response(request_wire(
            port, b"/api/submissions?class=13", b"POST", payload,
            b"Content-Type: application/octet-stream\r\nX-Trace: preserved\r\n"
            b"Connection: X-Hop\r\nX-Hop: remove\r\n", fragments=True), "POST")
        assert status == 201 and body == payload
        assert header_values(headers, "X-Backend-ID") == ["submission"]
        assert header_values(headers, "X-Received-Host") == [
            f"127.0.0.1:{fixture.ports['submission']}"]
        assert header_values(headers, "X-Received-Target") == ["/api/submissions?class=13"]
        assert header_values(headers, "X-Received-Content-Type") == ["application/octet-stream"]
        assert header_values(headers, "X-Received-Trace") == ["preserved"]
        assert header_values(headers, "X-Received-Hop") == ["absent"]
        assert not header_values(headers, "X-Backend-Hop")
        assert header_values(headers, "Set-Cookie") == ["a=1", "b=2"]
        assert header_values(headers, "Via") == ["1.1 jungle-proxy"]
        print("[PASS] submission destination, 64KiB binary POST, status and headers")

        with socket.create_connection(("127.0.0.1", port), timeout=4) as client:
            client.sendall(b"POST /api/submissions HTTP/1.1\r\nHost: test\r\n"
                           b"Content-Length: 3\r\nExpect: 100-continue\r\n\r\n")
            interim = b""
            while not interim.endswith(b"\r\n\r\n"):
                interim += client.recv(1)
            assert interim == b"HTTP/1.1 100 Continue\r\n\r\n"
            client.sendall(b"a\0b")
            client.shutdown(socket.SHUT_WR)
            status, headers, body = parse_response(receive_to_eof(client), "POST")
            assert status == 201 and body == b"a\0b"
            assert header_values(headers, "X-Received-Expect") == ["absent"]
        print("[PASS] Expect terminated at proxy, exact POST body forwarded")

        for target, method, expected in (
            (b"/", b"GET", 200), (b"/hello", b"GET", 200),
            (b"/cafe", b"GET", 200), (b"/greet?name=Alice", b"GET", 200),
            (b"/api/submissions", b"GET", 405),
            (b"/api/submission-status", b"POST", 405),
        ):
            assert parse_response(request_wire(port, target, method))[0] == expected
        malformed = [
            (b"POST /api/submissions HTTP/1.1\r\nHost: x\r\nContent-Length: 65537\r\n\r\n", 413),
            (b"POST /api/submissions HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n", 400),
            (b"POST /api/submissions HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n", 501),
            (b"GET /" + b"a" * 8200 + b" HTTP/1.1\r\nHost: x\r\n\r\n", 431),
        ]
        for wire, expected in malformed:
            with socket.create_connection(("127.0.0.1", port), timeout=4) as client:
                client.sendall(wire)
                client.shutdown(socket.SHUT_WR)
                assert parse_response(receive_to_eof(client))[0] == expected
        with socket.create_connection(("127.0.0.1", port), timeout=4) as client:
            client.sendall(b"GET /hello HTTP/1.0\r\nHost: original.example\r\n\r\n")
            client.shutdown(socket.SHUT_WR)
            assert parse_response(receive_to_eof(client))[0] == 505
        print("[PASS] local routes, request policies and HTTP/1.1-only client requests")

        for mode, expected, expected_body in (
            ("chunked", 200, b"a\0bcd"), ("close", 200, b"close\0body"),
            ("informational", 200, b"status\0ready\xff"),
            ("no-content", 204, b""), ("not-modified", 304, b""),
            ("http10", 200, b"hello"), ("http10-close", 200, b"close\0body"),
            ("http10-custom", 409, b"conflict"),
            ("http10-no-content", 204, b""), ("http10-not-modified", 304, b""),
            ("http10-informational", 502, None), ("http10-te", 502, None),
            ("bad-length", 502, None), ("reset", 502, None),
            ("silent", 504, None),
        ):
            fixture.query_mode(mode)
            wire = request_wire(port, timeout=3 if mode == "silent" else 8)
            status, headers, body = parse_response(wire)
            assert status == expected, (mode, status, wire)
            assert wire.startswith(b"HTTP/1.1 "), (mode, wire)
            if mode == "http10-close":
                assert header_values(headers, "Transfer-Encoding") == ["chunked"]
                assert not header_values(headers, "Content-Length")
            if mode == "http10":
                assert header_values(headers, "Content-Length") == ["5"]
                assert header_values(headers, "Content-Type") == ["text/plain"]
            if mode == "http10-custom":
                assert wire.startswith(b"HTTP/1.1 409 Legacy conflict\r\n")
            if mode == "http10-no-content":
                assert not header_values(headers, "Content-Type")
                assert not header_values(headers, "Content-Length")
            if mode == "http10-not-modified":
                assert header_values(headers, "Content-Length") == ["123"]
            if expected_body is not None:
                assert body == expected_body, (mode, body)
            if mode == "informational":
                interim_header, final_wire = wire.split(b"\r\n\r\n", 1)
                assert interim_header.startswith(b"HTTP/1.1 103 Early Hints\r\n")
                assert b"Link: </a.css>\r\n" in interim_header
                assert b"connection:" not in interim_header.lower()
                assert b"Via: 1.1 jungle-proxy" in interim_header
                assert final_wire.startswith(b"HTTP/1.1 200 ")
                assert header_values(headers, "Connection") == ["close"]
            if mode == "chunked":
                assert b"X-End: complete\r\n" in wire
            assert fixture.proxy.poll() is None
        fixture.query_mode("normal")
        fixture.healthy_query()
        print("[PASS] backend framing, 1xx/no-body responses, 502 and silent 504")

        for role in ("query-a", "query-b"):
            stop_child(fixture.backends[role])
        assert parse_response(request_wire(port))[0] == 502
        fixture.query_mode("normal")
        fixture.healthy_query()
        print("[PASS] refused backend connection -> 502 and recovery")

        time.sleep(0.05)  # Allow final close after the client observes write shutdown.
        baseline = fixture.fd_count()
        for mode in ("truncate", "http10-truncate", "bad-chunk", "body-timeout", "body-total"):
            fixture.query_mode(mode)
            wire = request_wire(port, timeout=20)
            assert wire.startswith(b"HTTP/1.1 200 "), (mode, wire)
            assert wire.count(b"HTTP/1.1 ") == 1, (mode, wire)
            try:
                parse_response(wire)
            except (http.client.IncompleteRead, http.client.HTTPException, ValueError):
                pass
            else:
                raise AssertionError(f"{mode}: incomplete response appeared complete")
            fixture.query_mode("normal")
            fixture.healthy_query()
        print("[PASS] truncated/invalid/timed-out bodies remain detectably incomplete")

        for _ in range(3):
            client = socket.create_connection(("127.0.0.1", port), timeout=4)
            client.sendall(b"GET /api/submission-status HTTP/1.1\r\nHost: x\r\n\r\n")
            client.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
            client.close()
            fixture.healthy_query()
        if baseline is None:
            print("[SKIP] FD count: /proc is unavailable")
        else:
            for _ in range(40):
                if fixture.fd_count() == baseline:
                    break
                time.sleep(0.05)
            assert fixture.fd_count() == baseline
            print("[PASS] repeated failures/client resets reclaim proxy FDs")
        for stop_signal in (signal.SIGINT, signal.SIGTERM):
            fixture.query_mode("silent")
            client = socket.create_connection(("127.0.0.1", port), timeout=4)
            client.sendall(b"GET /api/submission-status HTTP/1.1\r\nHost: x\r\n\r\n")
            time.sleep(0.1)
            fixture.proxy.send_signal(stop_signal)
            assert fixture.proxy.wait(timeout=6) == 0
            client.close()
            if stop_signal == signal.SIGINT:
                fixture.proxy_port = pick_free_port()
                port = fixture.proxy_port
                fixture.proxy = fixture.start(
                    [str(SERVER), str(port), str(fixture.config)], port)
        print("[PASS] SIGINT/SIGTERM during backend wait")
    print("Iteration 4 functional verification finished; no performance measurements.")


if __name__ == "__main__":
    main()