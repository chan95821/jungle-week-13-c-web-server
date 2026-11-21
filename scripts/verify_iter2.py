#!/usr/bin/env python3
"""Basic Iteration 2 functional checks. No performance metrics or load tests."""
import socket
import subprocess
import time

from verify_iter1 import pick_free_port, start_server, stop_server


class ResponseReader:
    def __init__(self, sock):
        self.sock = sock
        self.buffer = b""

    def _receive(self):
        data = self.sock.recv(4096)
        if not data:
            raise AssertionError("EOF before complete HTTP response")
        self.buffer += data

    def read(self):
        while b"\r\n\r\n" not in self.buffer:
            self._receive()
        header, self.buffer = self.buffer.split(b"\r\n\r\n", 1)
        lines = header.split(b"\r\n")
        status = int(lines[0].split(b" ", 2)[1])
        fields = {}
        for line in lines[1:]:
            name, value = line.split(b":", 1)
            name = name.lower()
            assert name not in fields, "Duplicate response field"
            fields[name] = value.strip()
        if 100 <= status < 200:
            assert b"content-length" not in fields
            length = 0
        else:
            assert b"content-length" in fields
            length = int(fields[b"content-length"])
        while len(self.buffer) < length:
            self._receive()
        body, self.buffer = self.buffer[:length], self.buffer[length:]
        return status, fields, body

    def assert_closed(self):
        assert not self.buffer, "Unexpected second response/data"
        assert self.sock.recv(1) == b"", "Expected Connection: close"


def request(port, wire, expected_status, expected_body=None, expected_type=None, fragments=False, allow_continue=False):
    with socket.create_connection(("127.0.0.1", port), timeout=3) as sock:
        if fragments:
            marker = wire.index(b"\r\n\r\n")
            pieces = [wire[:marker + 3], wire[marker + 3:marker + 4],
                      wire[marker + 4:marker + 6], wire[marker + 6:]]
            for part in pieces:
                if part:
                    sock.sendall(part)
                    time.sleep(0.01)
        else:
            sock.sendall(wire)
        sock.shutdown(socket.SHUT_WR)
        reader = ResponseReader(sock)
        status, headers, body = reader.read()
        if allow_continue and status == 100:
            status, headers, body = reader.read()
        assert status == expected_status, (status, expected_status, body)
        assert headers[b"connection"].lower() == b"close"
        if expected_body is not None:
            assert body == expected_body, "Body byte mismatch"
        if expected_type is not None:
            assert headers[b"content-type"] == expected_type
        reader.assert_closed()
        return headers


def main():
    port = pick_free_port()
    proc = start_server(port)
    try:
        for target in (b"/", b"/hello", b"/hello?x=%20"):
            request(port, b"GET " + target + b" HTTP/1.1\r\nHost: localhost\r\n\r\n",
                    200, b"Hello", b"text/plain")
        print("[PASS] GET root, hello, and query routing")

        binary = b"\x00hello\r\n\r\nGET /not-a-request\xff"
        header = (b"POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Type: text/html\r\n"
                  b"Content-Length: " + str(len(binary)).encode() + b"\r\n\r\n")
        request(port, header + binary, 200, binary, b"application/octet-stream")
        request(port, header + binary, 200, binary, b"application/octet-stream", fragments=True)
        request(port, b"POST /echo HTTP/1.1\r\nHost: localhost\r\n\r\n", 200, b"", b"application/octet-stream")
        request(port, b"POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\nignored",
                200, b"", b"application/octet-stream")
        request(port, b"POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 3\r\n\r\nabc"
                b"GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n", 200, b"abc", b"application/octet-stream")
        print("[PASS] binary, empty, fragmented, and extra-data body boundaries")

        for size in (65535, 65536):
            data = b"X" * size
            header = (b"POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: "
                      + str(size).encode() + b"\r\n\r\n")
            request(port, header + data, 200, data, b"application/octet-stream")
        request(port, b"POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 65537\r\n\r\n", 413)
        for size in (8191, 8192, 8193):
            prefix = b"GET / HTTP/1.1\r\nHost: localhost\r\nX-Fill: "
            wire = prefix + b"a" * (size - len(prefix) - 4) + b"\r\n\r\n"
            request(port, wire, 200 if size <= 8192 else 431)
        print("[PASS] selected header and body size boundaries")

        bad_requests = [
            (b"GET / HTTP/1.1\r\n\r\n", 400),
            (b"GET /bad%xy HTTP/1.1\r\nHost: a\r\n\r\n", 400),
            (b"POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: x\r\n\r\n", 400),
            (b"POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 2\r\nContent-Length: 2\r\n\r\n", 400),
            (b"POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n", 400),
            (b"POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n\r\nab", 400),
            (b"POST /echo HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n", 501),
            (b"POST /echo HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: gzip\r\n\r\n", 400),
            (b"POST /echo HTTP/1.1\r\nHost: a\r\nExpect: unsupported\r\n\r\n", 417),
            (b"GET /missing HTTP/1.1\r\nHost: a\r\n\r\n", 404),
            (b"DELETE /hello HTTP/1.1\r\nHost: a\r\n\r\n", 501),
            (b"GET /hello HTTP/1.0\r\nHost: a\r\n\r\n", 505),
            (b"GET http://localhost/hello HTTP/1.1\r\nHost: a\r\n\r\n", 501),
        ]
        for wire, expected in bad_requests:
            request(port, wire, expected)
        for method, path, allowed in ((b"GET", b"/echo", b"POST"), (b"POST", b"/hello", b"GET")):
            headers = request(port, method + b" " + path + b" HTTP/1.1\r\nHost: a\r\n\r\n", 405)
            assert headers[b"allow"] == allowed
        print("[PASS] basic malformed/unsupported requests and method dispatch")

        expect = b"POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 3\r\nExpect: 100-continue\r\n\r\n"
        with socket.create_connection(("127.0.0.1", port), timeout=3) as sock:
            sock.sendall(expect)
            reader = ResponseReader(sock)
            status, _, body = reader.read()
            assert status == 100 and body == b""
            sock.sendall(b"a\x00b")
            sock.shutdown(socket.SHUT_WR)
            status, headers, body = reader.read()
            assert status == 200 and body == b"a\x00b"
            assert headers[b"content-type"] == b"application/octet-stream"
            reader.assert_closed()
        request(port, expect + b"abc", 200, b"abc", b"application/octet-stream", allow_continue=True)
        request(port, expect.replace(b"Content-Length: 3", b"Content-Length: 65537"), 413)
        print("[PASS] 100 Continue, already-sent body, and early rejection")
    finally:
        assert stop_server(proc) == 0
    print("Basic Iteration 2 functional checks passed. Timing/fault/load matrices are not included.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
