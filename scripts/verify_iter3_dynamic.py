#!/usr/bin/env python3
"""Iteration 3 dynamic serving, CGI, and HTTP/1.1 chunked E2E checks."""
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile

from verify_iter1 import SERVER_BIN, pick_free_port, start_server, stop_server
from proxy_test_config import regression_config

REPO_ROOT = Path(__file__).resolve().parent.parent


def read_http_response(sock):
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            raise AssertionError("EOF before response headers")
        buf += chunk
    head, rest = buf.split(b"\r\n\r\n", 1)
    lines = head.split(b"\r\n")
    status = int(lines[0].split(b" ", 2)[1])
    headers = {}
    for line in lines[1:]:
        name, val = line.split(b":", 1)
        headers[name.lower()] = val.strip()

    if headers.get(b"transfer-encoding", b"").lower() == b"chunked":
        assert b"content-length" not in headers, "Chunked response must not include Content-Length"
        body = bytearray()
        while True:
            while b"\r\n" not in rest:
                more = sock.recv(4096)
                if not more:
                    raise AssertionError("Truncated chunked response (missing terminator)")
                rest += more
            size_line, rest = rest.split(b"\r\n", 1)
            chunk_size = int(size_line, 16)
            if chunk_size == 0:
                while len(rest) < 2:
                    more = sock.recv(4096)
                    if not more:
                        raise AssertionError("Truncated terminating chunk")
                    rest += more
                assert rest[:2] == b"\r\n", rest[:2]
                rest = rest[2:]
                break
            while len(rest) < chunk_size + 2:
                more = sock.recv(4096)
                if not more:
                    raise AssertionError("Truncated chunk payload")
                rest += more
            body.extend(rest[:chunk_size])
            assert rest[chunk_size:chunk_size + 2] == b"\r\n"
            rest = rest[chunk_size + 2:]
        assert not rest, "Unexpected trailing bytes after 0-chunk"
        assert sock.recv(1) == b"", "Expected Connection: close"
        return status, headers, bytes(body), True

    length = int(headers[b"content-length"])
    while len(rest) < length:
        more = sock.recv(4096)
        if not more:
            raise AssertionError("Truncated fixed-length response")
        rest += more
    assert len(rest) == length, "Unexpected extra bytes"
    assert sock.recv(1) == b"", "Expected Connection: close"
    return status, headers, rest, False


def send_req(port, target, method=b"GET", body=b""):
    wire = (method + b" " + target + b" HTTP/1.1\r\nHost: localhost\r\n"
            + b"Content-Length: " + str(len(body)).encode() + b"\r\n\r\n" + body)
    with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
        sock.settimeout(5)
        sock.sendall(wire)
        sock.shutdown(socket.SHUT_WR)
        return read_http_response(sock)


def send_raw_expect_incomplete_chunked(port, target):
    wire = b"GET " + target + b" HTTP/1.1\r\nHost: localhost\r\n\r\n"
    with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
        sock.settimeout(5)
        sock.sendall(wire)
        sock.shutdown(socket.SHUT_WR)
        chunks = []
        while True:
            data = sock.recv(4096)
            if not data:
                break
            chunks.append(data)
        raw = b"".join(chunks)
        assert b"Transfer-Encoding: chunked\r\n" in raw
        assert b"\r\n0\r\n\r\n" not in raw, raw


def test_repo_demo():
    port = pick_free_port()
    proc = start_server(port)
    try:
        st, hdrs, body, chunked = send_req(port, b"/cafe")
        assert st == 200 and not chunked and b'action="/greet"' in body
        assert b"/assets/cafe.js" in body

        st, hdrs, body, chunked = send_req(port, b"/menu")
        assert st == 200 and not chunked and b'action="/cgi-bin/order"' in body

        st, hdrs, body, chunked = send_req(port, b"/assets/cafe.js")
        assert st == 200 and not chunked
        assert hdrs[b"content-type"].split(b";", 1)[0].strip().lower() == b"text/javascript"
        assert b"fetch(" in body

        # Dynamic /greet checks
        st, hdrs, body, chunked = send_req(port, b"/greet")
        assert st == 200 and not chunked
        assert hdrs[b"content-type"].lower() == b"text/html; charset=utf-8"
        assert "어서 오세요, 손님님!".encode() in body

        st, _, body, _ = send_req(port, b"/greet?%6E%61%6D%65=%ED%99%8D+%EA%B8%B8%EB%8F%99")
        assert st == 200 and "어서 오세요, 홍 길동님!".encode() in body

        st, _, body, _ = send_req(port, b"/greet?name=%3Cscript%3Ealert(1)%3C%2Fscript%3E")
        assert st == 200 and b"&lt;script&gt;alert(1)&lt;/script&gt;" in body
        assert b"<script>alert(1)</script>" not in body

        st, _, body, _ = send_req(port, b"/greet?name=IgnoredBody", body=b"abc")
        assert st == 200 and "어서 오세요, IgnoredBody님!".encode() in body

        for bad in (b"/greet?name=Alice%00", b"/greet?other=%00&name=Alice", b"/greet?name=a%0Ab"):
            st, _, _, _ = send_req(port, bad)
            assert st == 400, bad

        st, hdrs, _, _ = send_req(port, b"/greet", method=b"POST")
        assert st == 405 and hdrs[b"allow"] == b"GET"
        st, _, _, _ = send_req(port, b"/greet/extra")
        assert st == 404

        # Dynamic CGI /cgi-bin/order checks
        st, hdrs, body, chunked = send_req(port, b"/cgi-bin/order?item=latte&qty=2")
        assert st == 200 and chunked
        assert hdrs[b"content-type"].lower() == b"text/html; charset=utf-8"
        assert "카페 라테".encode() in body and "9000원".encode() in body

        st, hdrs, body, chunked = send_req(port, b"/cgi-bin/order?item=bad&qty=1")
        assert st == 400 and chunked
        assert "주문 항목을 확인할 수 없습니다".encode() in body

        st, hdrs, _, _ = send_req(port, b"/cgi-bin/order", method=b"POST")
        assert st == 405 and hdrs[b"allow"] == b"GET"

        st, _, _, _ = send_req(port, b"/cgi-bin/%6frder")
        assert st == 400
        for bad404 in (b"/cgi-bin", b"/cgi-bin/", b"/cgi-bin/../public/pages/cafe.html",
                       b"/cgi-bin/order/extra", b"/cgi-bin/missing"):
            st, _, _, _ = send_req(port, bad404)
            assert st == 404, bad404

        print("[PASS] dynamic /greet, JS fetch assets, and chunked /cgi-bin/order demo")
    finally:
        stop_server(proc)


def test_isolated_cgi_faults():
    server_bin = (REPO_ROOT / SERVER_BIN).resolve()
    with tempfile.TemporaryDirectory(prefix="verify-iter3-dyn-") as tmp:
        root = Path(tmp)
        shutil.copytree(REPO_ROOT / "public", root / "public")
        port = pick_free_port()
        proc = subprocess.Popen([str(server_bin), str(port), regression_config()], cwd=root,
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            # Wait for port
            for _ in range(60):
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                        break
                except OSError:
                    import time
                    time.sleep(0.05)
            st, _, _, _ = send_req(port, b"/cgi-bin/order?item=latte&qty=1")
            assert st == 404
            st, _, _, _ = send_req(port, b"/greet?name=Alice")
            assert st == 200
        finally:
            stop_server(proc)

        cgi_dir = root / "cgi-bin"
        cgi_dir.mkdir()
        bad = cgi_dir / "bad.sh"
        bad.write_text("#!/bin/sh\nexit 1\n")
        bad.chmod(0o700)
        mid = cgi_dir / "mid.sh"
        mid.write_text("#!/bin/sh\nprintf 'Content-Type: text/plain\\n\\nhello'\nexit 1\n")
        mid.chmod(0o700)
        fds = cgi_dir / "fds.sh"
        fds.write_text(
            "#!/bin/sh\n"
            "printf 'Content-Type: text/plain\\n\\n'\n"
            "if [ -d /proc/$$/fd ]; then\n"
            "  ls -l /proc/$$/fd\n"
            "else\n"
            "  printf 'no-procfs\\n'\n"
            "fi\n"
        )
        fds.chmod(0o700)

        port = pick_free_port()
        proc = subprocess.Popen([str(server_bin), str(port), regression_config()], cwd=root,
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            for _ in range(60):
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                        break
                except OSError:
                    import time
                    time.sleep(0.05)
            st, _, _, _ = send_req(port, b"/cgi-bin/bad.sh")
            assert st == 502
            send_raw_expect_incomplete_chunked(port, b"/cgi-bin/mid.sh")
            st, _, fds_body, chunked = send_req(port, b"/cgi-bin/fds.sh")
            assert st == 200 and chunked
            if b"no-procfs" not in fds_body:
                assert b"socket:" not in fds_body, fds_body
            print("[PASS] missing cgi-bin startup, 502 error, FD_CLOEXEC, and incomplete chunk abort")
        finally:
            stop_server(proc)


def main():
    test_repo_demo()
    test_isolated_cgi_faults()
    print("All Iteration 3 dynamic, CGI, and chunked checks passed.")


if __name__ == "__main__":
    main()
