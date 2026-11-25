#!/usr/bin/env python3
"""B 기능/수명 검증. C 백엔드만 사용하며 처리량·p95·성능 비교를 수집하지 않는다."""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from contextlib import ExitStack, contextmanager
import http.client
from pathlib import Path
import shutil
import signal
import socket
import tempfile
import time

import verify_iter4 as regression
from verify_iter1 import receive_to_eof
from verify_iter4 import Fixture, ROOT, header_values, parse_response, request_wire, stop_child


@contextmanager
def running_fixture(workers):
    with ExitStack() as stack:
        directory = Path(stack.enter_context(tempfile.TemporaryDirectory(prefix="jungle-b-")))
        (directory / "public").symlink_to(ROOT / "public", target_is_directory=True)
        (directory / "cgi-bin").mkdir()
        shutil.copy2(ROOT / "cgi-bin/order", directory / "cgi-bin/order")
        fixture = Fixture(stack, directory)
        stop_child(fixture.proxy)
        fixture.proxy = fixture.start(
            [str(regression.SERVER), str(fixture.proxy_port), str(fixture.config), str(workers)],
            fixture.proxy_port)
        yield fixture


def check_submission(fixture, body=b"submission\0binary\xff"):
    status, headers, received = parse_response(request_wire(
        fixture.proxy_port, b"/api/submissions", b"POST", body), "POST")
    assert status == 201 and received == body, (status, len(received))
    assert header_values(headers, "X-Backend-ID") == ["submission"]


def check_slow_client_upload(fixture):
    payload = bytes(range(256)) * 256
    with socket.create_connection(("127.0.0.1", fixture.proxy_port), timeout=3) as slow:
        slow.settimeout(4)
        slow.sendall(b"POST /api/submissions HTTP/1.1\r\nHost: test\r\n"
                     b"Content-Length: 65536\r\nExpect: 100-continue\r\n\r\n")
        interim = bytearray()
        while b"\r\n\r\n" not in interim:
            chunk = slow.recv(1024)
            assert chunk, "EOF before 100 Continue"
            interim.extend(chunk)
        assert bytes(interim) == b"HTTP/1.1 100 Continue\r\n\r\n"
        slow.sendall(payload[:7])
        # 첫 요청 본문이 미완성이어도 다른 요청이 먼저 완료되어야 한다.
        check_submission(fixture, b"independent")
        slow.sendall(payload[7:])
        slow.shutdown(socket.SHUT_WR)
        status, _, received = parse_response(receive_to_eof(slow), "POST")
        assert status == 201 and received == payload


def check_global_query_sequence(fixture):
    def query(index):
        target = b"/api/submission-status" if index % 2 else b"/api/hello"
        status, headers, body = parse_response(request_wire(fixture.proxy_port, target))
        assert status == 200
        assert body == (b"status\0ready\xff" if index % 2 else b"Hello")
        return header_values(headers, "X-Backend-ID")[0]

    with ThreadPoolExecutor(max_workers=8) as clients:
        counts = Counter(clients.map(query, range(32)))
    # 완료 순서를 A/B 교대로 요구하지 않는다. 선택 횟수만 확인한다.
    assert counts == Counter({"query-a": 16, "query-b": 16}), counts


def check_waiting_backend_isolation(fixture):
    fixture.query_mode("slow-body")
    try:
        with ThreadPoolExecutor(max_workers=1) as reader:
            slow = reader.submit(request_wire, fixture.proxy_port)
            # 느린 본문 전송이 시작된 것을 socket으로 직접 관찰해 race를 없앤다.
            # 위 요청은 완료 전까지 2초 이상 걸리는 C fixture다.
            time.sleep(0.15)
            check_submission(fixture, b"while-backend-waits")
            assert not slow.done(), "submission was held until the slow query completed"
            assert parse_response(slow.result(timeout=5))[2] == b"S" * 2000
    finally:
        fixture.query_mode("normal")


def check_backpressure_and_fragments(fixture):
    fixture.query_mode("large")
    try:
        with socket.create_connection(("127.0.0.1", fixture.proxy_port), timeout=4) as slow:
            slow.settimeout(8)
            slow.sendall(b"GET /api/submission-status HTTP/1.1\r\nHost: test\r\n\r\n")
            slow.shutdown(socket.SHUT_WR)
            prefix = slow.recv(512)
            assert prefix.startswith(b"HTTP/1.1 200 ")
            # 이 소켓을 읽지 않는 동안에도 제출 요청은 완료되어야 한다.
            check_submission(fixture, b"while-output-waits")
            status, _, body = parse_response(prefix + receive_to_eof(slow))
            assert status == 200 and body == bytes(range(256)) * (8388608 // 256)
    finally:
        fixture.query_mode("normal")
    fixture.query_mode("fragmented-chunk")
    try:
        wire = request_wire(fixture.proxy_port)
        assert parse_response(wire)[2] == b"a\0bcd"
        assert b"X-End: complete\r\n" in wire
    finally:
        fixture.query_mode("normal")


def check_fault_cleanup(fixture):
    baseline = fixture.fd_count()
    fixture.restart_backend("submission", "truncate")
    try:
        for _ in range(12):
            wire = request_wire(fixture.proxy_port, b"/api/submissions", b"POST", b"x")
            assert wire.count(b"HTTP/1.1 ") == 1 and b"502 Bad Gateway" not in wire
            try:
                parse_response(wire, "POST")
            except http.client.IncompleteRead:
                pass
            else:
                raise AssertionError("truncated body looked complete")
    finally:
        fixture.restart_backend("submission")
    check_submission(fixture)
    deadline = time.monotonic() + 3
    while baseline is not None and fixture.fd_count() > baseline and time.monotonic() < deadline:
        time.sleep(0.05)
    assert baseline is None or fixture.fd_count() <= baseline, (baseline, fixture.fd_count())


def check_local_io_and_shutdown(fixture):
    script = fixture.directory / "cgi-bin/b-wait.sh"
    script.write_text("#!/bin/sh\nprintf 'Content-Type: text/plain\\r\\n\\r\\n'\nsleep 1\nprintf done\n")
    script.chmod(0o755)
    with ThreadPoolExecutor(max_workers=1) as clients:
        slow = clients.submit(request_wire, fixture.proxy_port, b"/cgi-bin/b-wait.sh")
        time.sleep(0.1)
        check_submission(fixture, b"while-cgi-waits")
        assert not slow.done(), "CGI blocked the network event worker"
        assert parse_response(slow.result(timeout=4))[2] == b"done"
    # stdout EOF 뒤에도 살아 있는 CGI의 waitpid가 영구 정지를 만들지 않아야 한다.
    script.write_text("#!/bin/sh\nprintf 'Content-Type: text/plain\\r\\n\\r\\n'\nexec 1>&-\nexec sleep 30\n")
    with socket.create_connection(("127.0.0.1", fixture.proxy_port), timeout=3) as pending:
        pending.sendall(b"GET /cgi-bin/b-wait.sh HTTP/1.1\r\nHost: x\r\n\r\n")
        assert pending.recv(1024).startswith(b"HTTP/1.1 200 ")
        fixture.proxy.send_signal(signal.SIGINT)
        assert fixture.proxy.wait(timeout=6) == 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, default=regression.SERVER)
    options = parser.parse_args()
    regression.SERVER = options.server.resolve()
    for workers in (1, 4):
        with running_fixture(workers) as fixture:
            tasks = Path(f"/proc/{fixture.proxy.pid}/task")
            if tasks.exists():
                assert len(list(tasks.iterdir())) == 1 + workers + 2
            check_slow_client_upload(fixture)
            check_global_query_sequence(fixture)
            check_waiting_backend_isolation(fixture)
            check_backpressure_and_fragments(fixture)
            check_fault_cleanup(fixture)
            check_local_io_and_shutdown(fixture)
        print(f"B functional checks passed: {workers} network worker(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
