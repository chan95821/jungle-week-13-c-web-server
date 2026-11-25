#!/usr/bin/env python3
"""Iteration 5 concurrency, CLI, CGI, HoL isolation, and shutdown verification."""
from contextlib import ExitStack
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import tempfile
import threading
import time

from verify_iter1 import pick_free_port, receive_to_eof, wait_for_port
from verify_iter4 import (
    ROOT, SERVER, Fixture, header_values, parse_response, request_wire, stop_child
)


def send_raw_request(port, raw_wire, timeout=8):
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as client:
        client.settimeout(timeout)
        client.sendall(raw_wire)
        client.shutdown(socket.SHUT_WR)
        return receive_to_eof(client)


def check_cli_workers(fixture):
    port = str(pick_free_port())
    for bad in ("0", "-1", "65", "abc", "", "1x"):
        res = subprocess.run([str(SERVER), port, str(fixture.config), bad],
                             cwd=fixture.directory, capture_output=True, timeout=3)
        assert res.returncode != 0, f"expected failure for workers={bad!r}"
    res = subprocess.run([str(SERVER), port, str(fixture.config), "4", "extra"],
                         cwd=fixture.directory, capture_output=True, timeout=3)
    assert res.returncode != 0, "expected failure for extra CLI argument"

    for workers in ("1", "4", "8"):
        test_port = pick_free_port()
        proc = fixture.start([str(SERVER), str(test_port), str(fixture.config), workers],
                             test_port)
        status, _, body = parse_response(request_wire(test_port, b"/hello"))
        assert status == 200 and body == b"Hello"
        proc.send_signal(signal.SIGTERM)
        assert proc.wait(timeout=0.75) == 0


def check_hol_isolation(fixture):
    """K < W: 1 slow request must not block fast requests across remaining workers."""
    fixture.query_mode("delayed-header")
    try:
        slow_done = threading.Event()
        slow_result = {}

        def run_slow():
            t0 = time.monotonic()
            wire = request_wire(fixture.proxy_port, b"/api/submission-status", timeout=4)
            slow_result["status"] = parse_response(wire)[0]
            slow_result["elapsed"] = time.monotonic() - t0
            slow_done.set()

        t = threading.Thread(target=run_slow)
        t.start()
        time.sleep(0.08)

        t0 = time.monotonic()
        status_hello, _, body_hello = parse_response(request_wire(fixture.proxy_port, b"/hello"))
        status_cafe, _, _ = parse_response(request_wire(fixture.proxy_port, b"/cafe"))
        status_submit, _, body_submit = parse_response(
            request_wire(fixture.proxy_port, b"/api/submissions", b"POST", b"fast-post"))
        fast_elapsed = time.monotonic() - t0

        assert status_hello == 200 and body_hello == b"Hello"
        assert status_cafe == 200
        assert status_submit == 201 and body_submit == b"fast-post"
        assert not slow_done.is_set(), f"fast requests waited for slow backend ({fast_elapsed:.3f}s)"
        assert fast_elapsed < 0.4, f"fast requests too slow: {fast_elapsed:.3f}s"

        t.join(timeout=4)
        assert slow_done.is_set() and slow_result["status"] == 504
    finally:
        fixture.query_mode("normal")


def check_all_workers_occupied_limit(fixture):
    """K == W: with workers=2 both occupied, a 3rd fast request waits until one frees."""
    port2 = pick_free_port()
    proc2 = fixture.start([str(SERVER), str(port2), str(fixture.config), "2"], port2)
    try:
        socks = []
        for _ in range(2):
            s = socket.create_connection(("127.0.0.1", port2), timeout=4)
            s.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\n12345")
            socks.append(s)

        time.sleep(0.1)
        fast_result = {}

        def run_fast():
            t0 = time.monotonic()
            fast_result["res"] = parse_response(request_wire(port2, b"/hello", timeout=4))
            fast_result["elapsed"] = time.monotonic() - t0

        fast_thread = threading.Thread(target=run_fast)
        fast_thread.start()
        time.sleep(0.25)
        assert "res" not in fast_result, "3rd request bypassed 2-worker limit"

        socks[0].sendall(b"67890")
        echo_status, _, echo_body = parse_response(socks[0].makefile("rb").read(), "POST")
        assert echo_status == 200 and echo_body == b"1234567890"
        socks[0].close()

        fast_thread.join(timeout=2)
        assert fast_result["res"][0] == 200 and fast_result["res"][2] == b"Hello"
        assert fast_result["elapsed"] >= 0.2

        socks[1].sendall(b"67890")
        echo_status2, _, echo_body2 = parse_response(socks[1].makefile("rb").read(), "POST")
        assert echo_status2 == 200 and echo_body2 == b"1234567890"
        socks[1].close()
    finally:
        stop_child(proc2)


def check_concurrent_all_paths_and_fds(fixture):
    baseline_fds = fixture.fd_count()

    def worker_job(idx):
        p = fixture.proxy_port
        mode = idx % 6
        if mode == 0:
            st, _, body = parse_response(request_wire(p, b"/cafe"))
            assert st == 200 and len(body) > 0
        elif mode == 1:
            st, _, body = parse_response(request_wire(p, b"/greet?name=chan"))
            assert st == 200 and b"chan" in body
        elif mode == 2:
            st, _, body = parse_response(request_wire(p, b"/cgi-bin/order?item=latte&qty=2"))
            assert st == 200 and "카페 라테".encode("utf-8") in body and b"9000" in body
        elif mode == 3:
            payload = f"item-{idx}".encode()
            st, hdrs, body = parse_response(
                request_wire(p, b"/api/submissions", b"POST", payload), "POST")
            assert st == 201 and body == payload
            assert header_values(hdrs, "X-Backend-ID") == ["submission"]
        elif mode == 4:
            st, hdrs, body = parse_response(request_wire(p, b"/api/submission-status"))
            assert st == 200 and body == b"status\0ready\xff"
            return header_values(hdrs, "X-Backend-ID")[0]
        else:
            st, hdrs, _ = parse_response(request_wire(p, b"/api/hello?k=1"))
            assert st == 200
            return header_values(hdrs, "X-Backend-ID")[0]
        return None

    with ThreadPoolExecutor(max_workers=12) as pool:
        results = list(pool.map(worker_job, range(120)))

    backend_hits = [r for r in results if r is not None]
    assert len(backend_hits) == 40
    assert backend_hits.count("query-a") == 20 and backend_hits.count("query-b") == 20, backend_hits

    time.sleep(0.05)
    after_fds = fixture.fd_count()
    assert after_fds == baseline_fds, (baseline_fds, after_fds)

    # Concurrent error paths (400, 404, 502, 504) under load + FD reclamation + recovery.
    fixture.restart_backend("submission", "reset")
    fixture.query_mode("delayed-header")
    try:
        def error_job(idx):
            p = fixture.proxy_port
            bucket = idx % 10
            if bucket < 3:
                st, _, _ = parse_response(
                    send_raw_request(p, b"GET /hello HTTP/1.1\r\nConnection: close\r\n\r\n"))
                assert st == 400, f"expected 400, got {st}"
            elif bucket < 6:
                st, _, _ = parse_response(request_wire(p, b"/missing-concurrent"))
                assert st == 404, f"expected 404, got {st}"
            elif bucket < 9:
                st, _, _ = parse_response(
                    request_wire(p, b"/api/submissions", b"POST", b"err-body"), "POST")
                assert st == 502, f"expected 502, got {st}"
            else:
                st, _, _ = parse_response(request_wire(p, b"/api/hello?err=1", timeout=8))
                assert st == 504, f"expected 504, got {st}"

        with ThreadPoolExecutor(max_workers=12) as pool:
            list(pool.map(error_job, range(40)))
    finally:
        fixture.restart_backend("submission", "normal")
        fixture.query_mode("normal")

    time.sleep(0.05)
    after_error_fds = fixture.fd_count()
    assert after_error_fds == baseline_fds, (baseline_fds, after_error_fds)
    fixture.healthy_query()


def check_idle_and_active_sigterm(fixture):
    for sig in (signal.SIGINT, signal.SIGTERM):
        p = pick_free_port()
        proc = fixture.start([str(SERVER), str(p), str(fixture.config), "4"], p)
        proc.send_signal(sig)
        assert proc.wait(timeout=0.75) == 0


def check_sigterm_while_queued(fixture):
    """Worker occupied + multiple requests queued: SIGTERM exits <= 0.75s and closes queued FDs."""
    p = pick_free_port()
    proc = fixture.start([str(SERVER), str(p), str(fixture.config), "1"], p)
    sockets = []
    try:
        busy = socket.create_connection(("127.0.0.1", p), timeout=3)
        busy.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\n12345")
        sockets.append(busy)
        time.sleep(0.08)

        for _ in range(8):
            s = socket.create_connection(("127.0.0.1", p), timeout=3)
            s.settimeout(1.0)
            s.sendall(b"GET /hello HTTP/1.1\r\nHost: x\r\n\r\n")
            sockets.append(s)

        time.sleep(0.08)
        proc.send_signal(signal.SIGTERM)
        assert proc.wait(timeout=0.75) == 0

        for s in sockets:
            try:
                data = s.recv(1024)
                assert data == b"", f"unexpected payload after shutdown: {data!r}"
            except (ConnectionResetError, BrokenPipeError):
                pass
    finally:
        for s in sockets:
            s.close()
        stop_child(proc)


def check_sigterm_during_slow_cgi(fixture):
    """Slow CGI child blocked in wait_readable() must cancel and exit <= 0.75s on SIGTERM."""
    slow_script = fixture.directory / "cgi-bin" / "slow_cancel_test"
    slow_script.write_text(
        "#!/bin/sh\nprintf 'Content-Type: text/plain\\r\\n\\r\\nhello'\nexec sleep 5\n",
        encoding="ascii",
    )
    slow_script.chmod(0o755)
    p = pick_free_port()
    proc = fixture.start([str(SERVER), str(p), str(fixture.config), "2"], p)
    client = None
    try:
        client = socket.create_connection(("127.0.0.1", p), timeout=3)
        client.settimeout(3)
        client.sendall(b"GET /cgi-bin/slow_cancel_test HTTP/1.1\r\nHost: x\r\n\r\n")
        seen = bytearray()
        while b"hello" not in seen:
            chunk = client.recv(1024)
            assert chunk, f"CGI closed prematurely: {bytes(seen)!r}"
            seen.extend(chunk)

        proc.send_signal(signal.SIGTERM)
        assert proc.wait(timeout=0.75) == 0
    finally:
        if client is not None:
            client.close()
        stop_child(proc)
        if slow_script.exists():
            slow_script.unlink()


def check_queued_deadline_408(fixture):
    """Request queued past HEADER_DEADLINE_SECONDS (7s) receives 408 from accepted_at."""
    p = pick_free_port()
    proc = fixture.start([str(SERVER), str(p), str(fixture.config), "1"], p)
    try:
        # Hold the single worker for ~7.2s while sending 1 byte every 3.6s (< 5s idle timeout).
        busy = socket.create_connection(("127.0.0.1", p), timeout=12)
        busy.settimeout(12)
        busy.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\n12345")
        time.sleep(0.1)

        queued = socket.create_connection(("127.0.0.1", p), timeout=12)
        queued.settimeout(12)
        queued.sendall(b"GET /hello HTTP/1.1\r\nHost: x\r\n\r\n")

        time.sleep(3.5)
        busy.sendall(b"6")
        time.sleep(3.6)
        busy.sendall(b"7890")
        busy.shutdown(socket.SHUT_WR)
        st1, _, b1 = parse_response(busy.makefile("rb").read(), "POST")
        busy.close()
        assert st1 == 200 and b1 == b"1234567890"

        st2, _, _ = parse_response(queued.makefile("rb").read())
        queued.close()
        assert st2 == 408, f"expected 408 after >7s queue wait, got {st2}"
    finally:
        stop_child(proc)


def main():
    with tempfile.TemporaryDirectory(prefix="verify-iter5-") as temporary, ExitStack() as stack:
        directory = Path(temporary)
        shutil.copytree(ROOT / "public", directory / "public")
        shutil.copytree(ROOT / "cgi-bin", directory / "cgi-bin")
        fixture = Fixture(stack, directory)

        check_cli_workers(fixture)
        print("[PASS] CLI [workers] validation and startup")
        check_hol_isolation(fixture)
        print("[PASS] HoL isolation (K < W): fast requests complete without waiting")
        check_all_workers_occupied_limit(fixture)
        print("[PASS] C-model limit (K == W): 3rd request queues until worker frees")
        check_concurrent_all_paths_and_fds(fixture)
        print("[PASS] Concurrent normal (120 reqs, 50:50 A/B) & error paths (400/404/502/504), FD reclamation")
        check_idle_and_active_sigterm(fixture)
        print("[PASS] Idle SIGINT/SIGTERM shutdown <= 0.75s")
        check_sigterm_while_queued(fixture)
        print("[PASS] SIGTERM with busy worker + queued requests exits <= 0.75s and closes FDs")
        check_sigterm_during_slow_cgi(fixture)
        print("[PASS] SIGTERM during slow CGI streaming exits <= 0.75s")
        check_queued_deadline_408(fixture)
        print("[PASS] Queued request exceeding HEADER_DEADLINE_SECONDS (7s) receives 408")


if __name__ == "__main__":
    main()
