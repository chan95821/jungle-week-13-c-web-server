#!/usr/bin/env python3
"""P1-7 functional edge checks adapted from chan eda192c; no Python HTTP backends."""
from contextlib import ExitStack, contextmanager
import http.client
from pathlib import Path
import select
import shutil
import signal
import socket
import subprocess
import tempfile
import threading
import time

from verify_iter1 import pick_free_port, receive_to_eof
from verify_iter4 import (
    ROOT, SERVER, Fixture, header_values, parse_response, request_wire,
    stop_child, verify_protected_connection_options,
)


class SkipCheck(Exception):
    pass


def exchange(port, wire, half_close=True, timeout=8):
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as client:
        client.settimeout(timeout)
        client.sendall(wire)
        if half_close:
            client.shutdown(socket.SHUT_WR)
        return receive_to_eof(client)


def expect_response(port, wire, expected, body=None, **options):
    response = parse_response(exchange(port, wire, **options))
    assert response[0] == expected, (expected, response[0])
    if body is not None:
        assert response[2] == body, (expected, len(response[2]))
    return response


@contextmanager
def backend_mode(fixture, role, mode):
    fixture.restart_backend(role, mode)
    try:
        yield
    finally:
        fixture.restart_backend(role)


def backend_response(fixture, mode, expected, body=None, timeout=8):
    with backend_mode(fixture, "submission", mode):
        status, _, received = parse_response(request_wire(
            fixture.proxy_port, b"/api/submissions", b"POST", b"x",
            timeout=timeout), "POST")
        assert status == expected, (mode, status)
        if body is not None:
            assert received == body, (mode, len(received))


def streaming_after_rejection(fixture):
    # Observe the early response before flooding: bounded drain does not guarantee
    # response delivery when unread concurrent data triggers a TCP reset.
    with socket.create_connection(("127.0.0.1", fixture.proxy_port), timeout=2) as client:
        client.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\n"
                       b"Content-Length: 1048576\r\n\r\n")
        assert parse_response(receive_to_eof(client))[0] == 413

        def send_extra():
            try:
                for _ in range(16):
                    client.sendall(b"Z" * 65536)
            except OSError:
                pass  # Peer closure/reset is an expected end of this attempt.

        writer = threading.Thread(target=send_extra, daemon=True)
        writer.start()
        writer.join(timeout=3)
        if writer.is_alive():
            try:
                client.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            writer.join(timeout=1)
            raise AssertionError("bounded extra sender did not finish")
    assert fixture.proxy.poll() is None


def check_health_and_fds(fixture, baseline):
    expect_response(fixture.proxy_port, b"GET /hello HTTP/1.1\r\nHost: x\r\n\r\n",
                    200, b"Hello")
    if baseline is None:
        raise SkipCheck("health passed; /proc FD observation unavailable")
    for _ in range(40):
        if fixture.fd_count() == baseline:
            return
        time.sleep(0.05)
    assert fixture.fd_count() == baseline, "FDs accumulated after edge requests"


def partial_body(fixture):
    with backend_mode(fixture, "submission", "body-timeout"):
        wire = request_wire(fixture.proxy_port, b"/api/submissions", b"POST", b"x")
        assert wire.startswith(b"HTTP/1.1 200 ")
        assert wire.count(b"HTTP/1.1 ") == 1
        try:
            parse_response(wire, "POST")
        except http.client.IncompleteRead as error:
            assert error.partial == b"partial"
        else:
            raise AssertionError("stalled body appeared complete")


def selected_host_contract(fixture):
    status, fields, body = parse_response(request_wire(
        fixture.proxy_port, b"/api/submissions", b"POST", b"abc"), "POST")
    assert status == 201 and body == b"abc"
    assert header_values(fields, "X-Received-Host") == [
        f"127.0.0.1:{fixture.ports['submission']}"]


def legacy_backend(fixture):
    fixture.query_mode("http10")
    try:
        wire = request_wire(fixture.proxy_port)
        assert wire.startswith(b"HTTP/1.1 200 ")
        assert parse_response(wire)[2] == b"hello"
    finally:
        fixture.query_mode("normal")


def slow_header(fixture):
    fixture.query_mode("slow-header")
    try:
        # The C peer sends four bytes every 0.4s. The total-header deadline must
        # win, rather than being restarted for every successful recv.
        wire = request_wire(fixture.proxy_port, timeout=2.5)
        assert parse_response(wire)[0] == 504
    finally:
        fixture.query_mode("normal")


def signal_during_backend_wait(fixture):
    fixture.query_mode("notify-silent")
    auxiliary = None
    try:
        port = pick_free_port()
        auxiliary = fixture.start([str(SERVER), str(port), str(fixture.config)], port)
        with socket.create_connection(("127.0.0.1", port), timeout=3) as client:
            client.sendall(b"GET /api/submission-status HTTP/1.1\r\nHost: x\r\n\r\n")
            events, _, _ = select.select([fixture.backends["query-a"].stderr], [], [], 1)
            assert events, "C backend did not confirm request reception"
            assert fixture.backends["query-a"].stderr.readline() == b"request-received\n"
            auxiliary.send_signal(signal.SIGTERM)
            assert auxiliary.wait(timeout=0.75) == 0
    finally:
        if auxiliary is not None:
            stop_child(auxiliary)
        fixture.query_mode("normal")


def required_config_contract(fixture):
    bad = fixture.directory / "bad-edge.conf"
    bad.write_text("unknown 127.0.0.1:8001\n", encoding="ascii")
    port = str(pick_free_port())
    for arguments in (
        [], [port], ["0", str(fixture.config)],
        [port, str(fixture.directory / "missing.conf")], [port, str(bad)],
    ):
        result = subprocess.run([str(SERVER), *arguments], cwd=fixture.directory,
                                capture_output=True, timeout=3)
        assert result.returncode != 0, arguments
    expect_response(fixture.proxy_port, b"GET /hello HTTP/1.1\r\nHost: x\r\n\r\n",
                    200, b"Hello")


def main():
    outcomes = []

    def check(name, operation):
        try:
            operation()
        except SkipCheck as error:
            outcomes.append("SKIP")
            print(f"[SKIP] {name}: {error}", flush=True)
        except Exception as error:
            outcomes.append("FAIL")
            print(f"[FAIL] {name}: {type(error).__name__}: {error}", flush=True)
        else:
            outcomes.append("PASS")
            print(f"[PASS] {name}", flush=True)

    with tempfile.TemporaryDirectory(prefix="verify-edge-") as temporary, ExitStack() as stack:
        directory = Path(temporary)
        shutil.copytree(ROOT / "public", directory / "public")
        fixture = Fixture(stack, directory)
        port = fixture.proxy_port
        expect_response(port, b"GET /hello HTTP/1.1\r\nHost: x\r\n\r\n", 200, b"Hello")
        time.sleep(0.05)
        baseline = fixture.fd_count()
        malformed = [
            ("missing Host", b"GET / HTTP/1.1\r\n\r\n", 400, None),
            ("invalid method", b"G@T / HTTP/1.1\r\nHost: x\r\n\r\n", 400, None),
            ("lowercase method", b"get / HTTP/1.1\r\nHost: x\r\n\r\n", 501, None),
            ("empty method", b" / HTTP/1.1\r\nHost: x\r\n\r\n", 400, None),
            ("HTTP/1.0 client", b"GET / HTTP/1.0\r\nHost: x\r\n\r\n", 505, None),
            ("HTTP/2.0 client", b"GET / HTTP/2.0\r\nHost: x\r\n\r\n", 505, None),
            ("invalid version", b"GET / HTTP/1.x\r\nHost: x\r\n\r\n", 400, None),
            ("request-line suffix", b"GET / HTTP/1.1 junk\r\nHost: x\r\n\r\n", 400, None),
            ("missing colon", b"GET / HTTP/1.1\r\nHost: x\r\nBroken\r\n\r\n", 400, None),
            ("header-name space", b"GET / HTTP/1.1\r\nHost: x\r\nBad Name: x\r\n\r\n", 400, None),
            ("header control", b"GET / HTTP/1.1\r\nHost: x\r\nX: bad\x01value\r\n\r\n", 400, None),
            ("space before colon", b"GET / HTTP/1.1\r\nHost : x\r\n\r\n", 400, None),
            ("HEAD no body", b"HEAD / HTTP/1.1\r\nHost: x\r\n\r\n", 501, b""),
            ("unframed extra data", b"POST /echo HTTP/1.1\r\nHost: x\r\n\r\nabc", 200, b""),
        ]
        for number, (name, wire, expected, body) in enumerate(malformed, 1):
            check(f"E{number:02d} {name}", lambda wire=wire, expected=expected, body=body:
                  expect_response(port, wire, expected, body))
        check("E15 incomplete header EOF", lambda: expect_response(
            port, b"GET / HTTP/1.1\r\nHost: x\r\n", 400))
        check("E16 incomplete header idle", lambda: expect_response(
            port, b"GET / HTTP/1.1\r\nHost: x\r\n", 408, half_close=False))
        check("E17 incomplete body idle", lambda: expect_response(
            port, b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\nx",
            408, half_close=False))

        def empty_connection():
            with socket.create_connection(("127.0.0.1", port), timeout=2):
                pass
            expect_response(port, b"GET /hello HTTP/1.1\r\nHost: x\r\n\r\n", 200, b"Hello")

        check("E18 empty connection survival", empty_connection)
        check("E19 long URI within header cap", lambda: expect_response(
            port, b"GET /" + b"a" * 8001 + b" HTTP/1.1\r\nHost: x\r\n\r\n", 404))
        check("E20 long hello query", lambda: expect_response(
            port, b"GET /hello?" + b"q" * 7000 + b" HTTP/1.1\r\nHost: x\r\n\r\n", 200, b"Hello"))
        check("E21 URI exceeds header cap", lambda: expect_response(
            port, b"GET /" + b"a" * 8180 + b" HTTP/1.1\r\nHost: x\r\n\r\n", 431))
        payload = bytes(range(256)) * 256
        check("E22 64KiB echo", lambda: expect_response(
            port, b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 65536\r\n\r\n" + payload,
            200, payload))
        check("E23 65537 declaration", lambda: expect_response(
            port, b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 65537\r\n\r\n", 413))
        check("E24 10MiB declaration", lambda: expect_response(
            port, b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 10485760\r\n\r\n", 413))
        check("E25 bounded extra stream after early 413", lambda: streaming_after_rejection(fixture))
        check("E26 recovery and FD reclamation", lambda: check_health_and_fds(fixture, baseline))

        def proxy_binary():
            status, _, body = parse_response(request_wire(
                port, b"/api/submissions", b"POST", payload), "POST")
            assert status == 201 and body == payload

        check("E27 64KiB proxy POST", proxy_binary)
        check("E28 oversized proxy declaration", lambda: expect_response(
            port, b"POST /api/submissions HTTP/1.1\r\nHost: x\r\nContent-Length: 65537\r\n\r\n", 413))
        check("E29 slowly progressing body", lambda:
              backend_response(fixture, "slow-body", 201, b"S" * 2000))
        check("E30 delayed header uses selected 1s deadline", lambda:
              backend_response(fixture, "delayed-header", 504))
        check("E31 silent backend", lambda: backend_response(fixture, "silent", 504))
        check("E32 query survives submit timeout", fixture.healthy_query)
        check("E33 partial backend header", lambda:
              backend_response(fixture, "partial-header", 504))
        check("E34 body stall without second response", lambda: partial_body(fixture))
        check("C2 selected Host rewrite contract", lambda: selected_host_contract(fixture))
        check("C3 protected header early rejection", lambda: verify_protected_connection_options(fixture))
        check("C7 HTTP/1.0 backend normalization", lambda: legacy_backend(fixture))
        check("C12 total header deadline with ongoing bytes", lambda: slow_header(fixture))
        check("C13 SIGTERM after backend reception", lambda: signal_during_backend_wait(fixture))
        check("C14 selected required-config contract", lambda: required_config_contract(fixture))

    passed, failed, skipped = (outcomes.count(status) for status in ("PASS", "FAIL", "SKIP"))
    print(f"Edge checks: {passed} passed, {failed} failed, {skipped} skipped.", flush=True)
    print("Functional deadlines only; no load/performance measurements.", flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())