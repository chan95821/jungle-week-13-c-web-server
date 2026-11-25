# B 검증 후보 — 실행 허락 전 문서화

## 기존 기능 회귀

- 기존 8개 단위 테스트: 포트/HTTP/부분 수신·송신/본문 provider/라우팅/프록시 프로토콜/할당·FD 정리.
- 기존 Iteration 1~4와 edge E2E: 정적 파일, greet, CGI, 정확한 POST 바이너리, 응답 상태/헤더, HTTP 1.0 백엔드, interim·chunk·trailer·EOF, 502/504, 응답 중단.

## B 기능과 수명

- 작업자 1/4로 느린 요청 여러 개와 빠른 제출을 겹쳐 처리. 처리량 기록 없이 응답 완료/격리만 확인.
- 모든 작업자가 공유하는 A/B 순번: 병렬 요청의 개수로 확인하고 도착 순서와 완료 순서를 혼동하지 않는다.
- 분할 헤더/본문/100 Continue; 클라이언트 SHUT_WR 후 정상 응답; 느린 수신과 바이너리 응답.
- nonblocking connect 실패·무응답·고정 본문 중단·chunk 중단 뒤 정상 처리. 반복 전후 /proc/PID/fd 수가 정리 대기 후 누적되지 않는다.
- 읽기 대기/송신 대기/보조 CGI 작업 중 SIGINT/TERM. 작업자 join, CGI 자식 회수, FD/연결 수명 정리.
- ASan/UBSan은 메모리·정의되지 않은 동작, 별도 TSan 빌드는 자료 경쟁을 검사한다. sanitizer가 실행 환경에서 동작하지 않으면 미검증으로 기록한다.

## 통과 기준과 제외

바이트·상태·헤더 계약 유지, 상한과 오류 매핑 유지, 타임아웃 허용 오차 내 종료, 다른 요청 계속 처리, 장애 뒤 FD 비누적, sanitizer 오류 없음. 시간 제한의 기능 판정은 성능 비교가 아니다.

성능/부하·처리량/p95 수집, 10,000연결, 자원 포화 비교는 제외한다. 실행 명령과 최종 범위는 구현 후 사용자에게 제시해 확정한다. 현재 이 문서 작성은 실행 허락이 아니다.

## 준비한 실행 범위

1. `make test`: 기존 8개 + 실제 EAGAIN/부분 송신과 타이머 힙을 확인하는 `test_event_io` (ASan/UBSan).
2. `make verify`: 기존 6개 E2E 스크립트.
3. `make verify-b`: `scripts/verify_iter5.py`, 작업자 1/4에서 미완성 업로드·기다리는 백엔드·느린 수신·분할 chunk/trailer·전체 RR·장애 FD 정리·CGI 대기/종료.
4. `python3 scripts/verify_iter5.py --server build/server-asan`, 동일 기능 범위의 메모리 검사.
5. `python3 scripts/verify_iter5.py --server build/server-tsan`, 동일 기능 범위의 자료 경쟁 검사. sanitizer 실행 환경 오류는 통과로 처리하지 않는다.

예상 통과 기준: 각 스크립트 assertion 통과, 응답 바이트 일치, 502/504 유지, 중단 응답의 불완전성 검출, FD 비누적, 정상 종료, sanitizer 진단 없음. 실패 시 이 범위 내 수정 후 재검증한다. 단순 성공 로그만으로 stderr의 sanitizer 진단을 놓치지 않도록 검사한다.
