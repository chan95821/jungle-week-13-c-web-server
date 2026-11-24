#!/usr/bin/env python3
"""개인 개발용 백엔드. 팀 체크포인트의 공통 백엔드로 확정된 코드는 아니다.

서버 3개는 같은 파일을 각각 실행한다. 저장·동기화·측정 기능은 없다.
제출은 받은 바이트를 그대로 반환해 프록시의 본문 보존을 확인할 수 있게 하고,
조회는 모든 인스턴스에서 같은 고정 상태를 반환한다. 차이는 X-Backend-Id뿐이다.
표준 Python HTTP 서버는 테스트 도구에만 사용하며 제출할 C 서버와 구분한다.
"""

import argparse
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import urlsplit


class Backend(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def reply(self, status, body, content_type):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("X-Backend-Id", self.server.backend_id)
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)
        self.close_connection = True

    def do_POST(self):
        # 역할과 경로를 동시에 검사하므로 잘못된 목적지로 중계되면 성공하지 않는다.
        if self.server.role != "submission" or urlsplit(self.path).path != "/api/submissions":
            self.reply(404, b"Not Found", "text/plain")
            return
        lengths = self.headers.get_all("Content-Length", [])
        if len(lengths) != 1 or not lengths[0].isascii() or not lengths[0].isdigit():
            self.reply(400, b"Content-Length required", "text/plain")
            return
        length = int(lengths[0])
        if length > 65536:
            self.reply(413, b"Content Too Large", "text/plain")
            return
        # rfile.read는 명시적 길이를 사용한다. NUL을 포함해도 문자열로 해석하지 않는다.
        body = self.rfile.read(length)
        if len(body) != length:
            self.reply(400, b"Incomplete request", "text/plain")
            return
        self.reply(201, body, "application/octet-stream")

    def do_GET(self):
        if self.server.role != "status" or urlsplit(self.path).path != "/api/submission-status":
            self.reply(404, b"Not Found", "text/plain")
            return
        self.reply(200, b'{"status":"submitted"}\n', "application/json")

    def log_message(self, message, *args):
        # 목적지·원본 target을 눈으로 확인하는 개발 로그. 처리량 계측용이 아니다.
        print(f"[{self.server.backend_id}] {message % args}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--role", choices=("submission", "status"), required=True)
    parser.add_argument("--id", required=True)
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("port must be in 1..65535")
    if not args.id or any(ord(c) < 32 or ord(c) > 126 for c in args.id):
        parser.error("id must contain printable ASCII only")
    with HTTPServer(("127.0.0.1", args.port), Backend) as server:
        server.role = args.role
        server.backend_id = args.id
        # 불완전 요청이 테스트 백엔드 자체를 영원히 점유하지 않게 한다.
        get_request = server.get_request

        def accept_with_timeout():
            client, address = get_request()
            client.settimeout(5)
            return client, address

        server.get_request = accept_with_timeout
        print(f"{args.id}: 127.0.0.1:{args.port} ({args.role})", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
