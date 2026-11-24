#!/usr/bin/env python3
"""Iteration 4 개인 검증용 백엔드 mock. 저장소 산출물이 아니라 실행 도구다.

세 백엔드는 같은 코드를 쓰고 `--id`만 다르게 준다. 부하 형태(지연·무응답·절단·실패)를
옵션으로 바꿀 수 있어, 프록시의 정상 중계·타임아웃·부분 응답·장애 우회를 같은 도구로 재현한다.

예:
  python3 tools/mock_backend.py --port 8001 --id submissions
  python3 tools/mock_backend.py --port 8002 --id lookup-a
  python3 tools/mock_backend.py --port 8003 --id lookup-b --delay-ms 300
  python3 tools/mock_backend.py --port 8003 --id lookup-b --hang-rate 1 --hang-ms 10000
  python3 tools/mock_backend.py --port 8003 --id lookup-b --truncate-bytes 10 --body-bytes 100

확인용 엔드포인트(프록시를 거치지 않고 백엔드에 직접 접속):
  GET /__stats  → 요청 수·경로별 수·상태별 수·동시 처리 최대치
  POST /__reset → 통계 초기화
"""

import argparse
import json
import random
import socket
import sys
import threading
import time


class Stats:
    """백엔드가 실제로 받은 요청을 세어 분산·재시도 여부를 확인한다."""

    def __init__(self):
        self.lock = threading.Lock()
        self.total = 0
        self.paths = {}
        self.statuses = {}
        self.active = 0
        self.max_active = 0

    def enter(self):
        with self.lock:
            self.active += 1
            self.max_active = max(self.max_active, self.active)

    def leave(self):
        with self.lock:
            self.active -= 1

    def record(self, path, status):
        with self.lock:
            self.total += 1
            self.paths[path] = self.paths.get(path, 0) + 1
            self.statuses[str(status)] = self.statuses.get(str(status), 0) + 1

    def snapshot(self):
        with self.lock:
            return {
                "total": self.total,
                "paths": dict(self.paths),
                "statuses": dict(self.statuses),
                "max_concurrent": self.max_active,
            }

    def reset(self):
        with self.lock:
            self.total = 0
            self.paths.clear()
            self.statuses.clear()
            self.max_active = self.active


def read_headers(conn):
    """CRLFCRLF까지 읽어 헤더 블록과 이미 읽은 나머지 바이트를 함께 돌려준다."""
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = conn.recv(4096)
        if not chunk:
            return None, b""
        data += chunk
        if len(data) > 65536:
            return None, b""
    head, rest = data.split(b"\r\n\r\n", 1)
    return head, rest


def parse_request(head):
    lines = head.split(b"\r\n")
    method, target, _version = lines[0].decode("latin-1").split(" ", 2)
    path = target.split("?", 1)[0]
    headers = {}
    for line in lines[1:]:
        name, value = line.split(b":", 1)
        headers[name.strip().lower()] = value.strip()
    return method, target, path, headers


def read_body(conn, headers, already):
    length = int(headers.get(b"content-length", b"0") or b"0")
    body = bytearray(already)
    while len(body) < length:
        chunk = conn.recv(min(65536, length - len(body)))
        if not chunk:
            break
        body.extend(chunk)
    return bytes(body[:length])


def build_head(status, length, extra_headers=b""):
    reason = {200: "OK", 201: "Created", 204: "No Content", 500: "Internal Server Error"}.get(
        status, "OK")
    return (f"HTTP/1.1 {status} {reason}\r\n"
            f"Content-Type: application/json\r\n"
            f"Content-Length: {length}\r\n"
            f"{extra_headers}"
            f"Connection: close\r\n\r\n").encode()


def extra_headers(options):
    """--header로 받은 헤더를 응답 헤더 블록 조각으로 만든다."""
    return "".join(f"{line}\r\n" for line in options.header).encode()


def build_response(status, body, extra_headers=b""):
    return build_head(status, len(body), extra_headers) + body


def send_body(conn, body, options):
    """헤더는 전체 길이를 선언하고, 옵션에 따라 본문만 잘라 보내거나 조각내어 보낸다."""
    limit = len(body) if options.truncate_bytes is None \
        else min(options.truncate_bytes, len(body))
    if options.slow_body_ms <= 0:
        conn.sendall(body[:limit])
        return

    sent = 0
    while sent < limit:
        part = body[sent:sent + options.chunk_bytes]
        if not part:
            break
        conn.sendall(part)
        sent += len(part)
        if sent < limit:
            time.sleep(options.slow_body_ms / 1000.0)


def handle(conn, options, stats):
    stats.enter()
    try:
        conn.settimeout(10)
        head, rest = read_headers(conn)
        if head is None:
            return
        method, target, path, headers = parse_request(head)

        if path == "/__stats" and method == "GET":
            body = json.dumps(stats.snapshot()).encode()
            conn.sendall(build_response(200, body, extra_headers(options)))
            return
        if path == "/__reset" and method == "POST":
            stats.reset()
            conn.sendall(build_response(200, b'{"reset":true}', extra_headers(options)))
            return

        body = read_body(conn, headers, rest)

        # 처리 지연: 고정 지연 + 무작위 편차로 조회 부하를 단계적으로 올린다.
        send_delay = options.delay_ms / 1000.0
        if options.jitter_ms:
            send_delay += random.uniform(0, options.jitter_ms / 1000.0)

        # 무응답: 요청은 받았지만 응답을 보내지 않아 프록시 504 경로를 만든다.
        if options.hang_rate and random.random() < options.hang_rate:
            stats.record(path, 0)
            time.sleep(options.hang_ms / 1000.0)
            return

        # 실패 응답: 프록시가 백엔드의 정상 형식 5xx를 그대로 중계하는지 확인한다.
        if options.fail_rate and random.random() < options.fail_rate:
            if send_delay:
                time.sleep(send_delay)
            conn.sendall(build_response(500, b'{"error":"injected"}', extra_headers(options)))
            stats.record(path, 500)
            return

        if path == "/api/submissions" and method == "POST":
            status = 201
            payload = {"server": options.id, "received": len(body), "status": "queued"}
        elif path == "/api/submission-status":
            status = 200
            payload = {"server": options.id, "path": path, "status": "submitted"}
        elif path == "/api/hello":
            status = 200
            payload = {"server": options.id, "message": "hello from mock backend"}
        else:
            status = 200
            payload = {"server": options.id, "path": path}

        if options.binary_path and path == options.binary_path:
            encoded = bytes(i % 256 for i in range(options.binary_bytes))
        else:
            encoded = json.dumps(payload).encode()
        # 응답 크기를 키워 본문 중계·부분 응답 검증에 사용한다.
        if len(encoded) < options.body_bytes:
            encoded = encoded + b" " * (options.body_bytes - len(encoded))

        if send_delay:
            time.sleep(send_delay)

        conn.sendall(build_head(status, len(encoded), extra_headers(options)))
        send_body(conn, encoded, options)
        stats.record(path, status)
    except (OSError, ValueError):
        return
    finally:
        stats.leave()
        try:
            conn.close()
        except OSError:
            pass


def serve(options):
    stats = Stats()
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind((options.host, options.port))
    listener.listen(128)
    print(f"[mock] {options.id} on {options.host}:{options.port}", file=sys.stderr, flush=True)
    try:
        while True:
            conn, _addr = listener.accept()
            thread = threading.Thread(target=handle, args=(conn, options, stats), daemon=True)
            thread.start()
    except KeyboardInterrupt:
        pass
    finally:
        listener.close()


def parse_args():
    parser = argparse.ArgumentParser(description="Iteration 4 reverse proxy mock backend")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--id", required=True, help="응답에 표시할 백엔드 식별자")
    parser.add_argument("--delay-ms", type=int, default=0, help="응답 전 고정 지연")
    parser.add_argument("--jitter-ms", type=int, default=0, help="응답 전 0..N 무작위 추가 지연")
    parser.add_argument("--body-bytes", type=int, default=0, help="응답 본문 최소 크기")
    parser.add_argument("--fail-rate", type=float, default=0.0, help="500을 반환할 확률")
    parser.add_argument("--hang-rate", type=float, default=0.0, help="응답하지 않을 확률")
    parser.add_argument("--hang-ms", type=int, default=10000, help="무응답 유지 시간")
    parser.add_argument("--truncate-bytes", type=int, default=None,
                        help="Content-Length보다 적게 보낸 뒤 연결을 닫는다")
    parser.add_argument("--slow-body-ms", type=int, default=0,
                        help="본문을 조각 사이 간격을 두고 천천히 보낸다")
    parser.add_argument("--chunk-bytes", type=int, default=16384,
                        help="--slow-body-ms로 나눠 보낼 조각 크기")
    parser.add_argument("--header", action="append", default=[], metavar="'Name: value'",
                        help="모든 응답에 추가할 헤더(여러 번 지정 가능)")
    parser.add_argument("--binary-path", default=None,
                        help="이 경로는 JSON 대신 이진 패턴 본문으로 응답한다(예: /api/hello)")
    parser.add_argument("--binary-bytes", type=int, default=32,
                        help="--binary-path 응답 본문 크기")
    options = parser.parse_args()
    if not 0.0 <= options.fail_rate <= 1.0 or not 0.0 <= options.hang_rate <= 1.0:
        parser.error("--fail-rate/--hang-rate must be between 0 and 1")
    return options


if __name__ == "__main__":
    serve(parse_args())
