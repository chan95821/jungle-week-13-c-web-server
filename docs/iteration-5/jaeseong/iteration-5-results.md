# Iteration 5 B — 이번 대화에서 실행한 검증

날짜: 2026-11-25. 브랜치 `iteration5/jaeseong`. 시작 기준은 `678df2c`; 작업 중 사용자가 만든 커밋은 유지했다. 팀 채택/병합 완료를 뜻하지 않는다.

## 환경과 허락

- 사용자가 “검증해야지”로 제시된 기능·자원 검증 범위를 재개했다. 성능 측정은 별도다.
- Debian 13 WSL2, 커널 `5.15.167.4-microsoft-standard-WSL2`, GCC 14.2.0, Python 3.13.5, 프로세스 FD soft limit 1024, root 실행.
- 일반 서버 `-std=c11 -Wall -Wextra -Werror -O2 -pthread`. ASan/UBSan과 TSan은 별도 바이너리로 검사했다.
- 기존 Ubuntu의 `ext4.vhdx`가 없어 실행되지 않는다. 이 결과는 Ubuntu 실행 검증이 아니다.

## 실행 결과

| 검사 | 결과 |
|---|---|
| `make all backend` | 컴파일/링크 성공 |
| `make test` 기존 8개 + `test_event_io` | 9개 통과, ASan/UBSan 진단 없음 |
| Iteration 1/2 E2E | 통과 |
| Iteration 3 정적 E2E | 통과, 권한 거부 1항목 skip (root가 chmod 000 파일을 읽을 수 있음) |
| Iteration 3 CGI/동적 E2E | 통과: 기존 CGI, CLOEXEC, 불완전 chunk 정리 |
| Iteration 4 proxy E2E | 통과: 경로/본문/헤더/응답 프레이밍/502/504/장애 FD/신호 종료 |
| `make verify-edge` | 수정 후 40 통과, 0 실패, 0 skip |
| `make verify-b` | 네트워크 작업자 1/4 모두 통과 |
| B with `build/server-asan` | 작업자 1/4 모두 통과, ASan/UBSan/LSan 진단 없음 |
| B with `build/server-tsan` | 작업자 1/4 모두 통과, TSan 진단 없음 |
| `git diff --check` | 통과 |

부분 송신 단위 검사는 실제 작은 SO_SNDBUF로 EAGAIN을 발생시키고 prefix/바이너리 본문/suffix의 정확한 재개를 확인한다. B E2E는 64KiB 분할 업로드·100 Continue, 공유 A/B 순번, 기다리는 백엔드 옆의 제출, 읽지 않는 클라이언트 옆의 제출, 8MiB 바이너리, 글자 단위 분할 chunk/trailer, 본문 중단 반복 후 FD, CGI 대기 중 다른 요청/종료를 검사한다. 처리량·p95 수집이 아니다.

## 발견 후 수정한 사항

- 첫 기존 회귀 실행은 curl 미설치로 중단했다. 도구 설치 후 다시 실행했다. 이를 서버 기능 통과로 세지 않았다.
- 첫 edge 검사에서 미완성 본문 idle 사례가 실패했다. 전체 15초 기한만 옮기고 기존 5초 입력 idle 기한을 빠뜨린 것이 원인이다. 입력 진행 시에만 idle 기한을 갱신하고 전체 기한과 최솟값을 적용하도록 수정했다. 재검사 40/40 통과.
- 첫 TSan B 검사는 sanitizer 자체의 감시 스레드를 일반 작업자로 세어 스레드 수 assertion이 실패했다. 일반 서버에서는 N+3을 정확히 확인하고, sanitizer 서버는 자체 스레드가 있을 수 있게 분리했다. 재실행은 진단 없이 통과했다.
- stdout을 닫고 살아 있는 CGI의 waitpid가 무기한 대기하지 않도록 회수 대기를 제한했다. 종료 후에도 보조 작업 결과/cleanup 전 연결 메모리를 해제하지 않는다.

## 미검증·한계

- 비root 권한 거부, 실제 Ubuntu/macOS, 극한 포화·10,000연결, 메모리 할당 실패를 새 B 전체 경로에 주입하는 검사는 미검증이다. 기존 proxy 할당 실패 단위 테스트 통과와 구분한다.
- 한 번의 sanitizer 실행은 모든 스케줄에서 경쟁·누수가 없다는 수학적 보장이 아니다.
- B는 Linux epoll/eventfd/signalfd 사용. 파일 realpath/open 사이 경합은 기존 한계다. 파일/CGI 보조 풀의 크기·큐 상한도 공정 비교 조건에 포함해야 한다. 임의의 디스크 커널 I/O 중단의 즉시 취소는 보장하지 않는다.
- FD soft limit 1024는 실제 수용 연결 수를 제한한다. 코드의 작업자당 4096 상한을 실제 4096 연결 검증 결과로 보고하지 않는다. 시스템 한도를 자동 변경하지 않았다.
- 성능 측정과 팀 후보 채택은 미완료다. 별도 [측정 제안](iteration-5-performance-plan.md)을 제시했으며 아직 실행 결과가 없다.
