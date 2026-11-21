# 개발 진행 맥락

## 범위와 원칙

- 이 저장소는 C로 만드는 HTTP 웹 서버 및 Reverse Proxy다.
- 구현 순서는 GitHub Issues `#1`~`#6`의 Iteration 계획을 따른다.
- Proxy 범위는 **Reverse Proxy만**이다. Forward Proxy와 HTTPS `CONNECT` 터널링은 구현하지 않는다.
- 매 Iteration은 각 팀원이 설계, 구현, 검증을 독립적으로 수행한 뒤 체크포인트에서 비교하고 하나의 기준 구현으로 통합한다.
- `main`에는 해당 체크포인트의 회귀 테스트를 통과한 코드만 둔다.
- 구현·설계·검증의 공통 규칙은 [AGENTS.md](../AGENTS.md)를 따른다.

## Iteration 계획 (`#1`~`#6`)

각 Iteration의 목표, 구현 범위, 완료 조건(DoD), 설계 주의사항은 GitHub Issues `#1`~`#6`에 정의되어 있으므로 작업 전 해당 이슈를 확인한다.

```sh
gh issue list --state all
gh issue view <번호>
```

- **#1** (`Iteration 1 — TCP 기반 HTTP 응답`): `main`에 병합 완료된 기준선
- **#2 ~ #6**: 현재부터 순차 진행할 Iteration (각 단계의 상세 범위와 DoD는 해당 이슈 기준)

## 브랜치 전략

| 목적 | 이름 | 규칙 |
| --- | --- | --- |
| 검증된 기준선 | `main` | Iteration 통합 검증 후에만 병합한다. |
| Iteration 통합 | `iteration/<n>-<topic>` | 최신 `main`에서 만들고, 체크포인트 동안 후보 구현을 검토·통합한다. |
| 개인 구현 | `team/<name>/iteration-<n>-<topic>` | 해당 Iteration 통합 브랜치에서 분기한다. 설계·코드·검증 근거를 함께 남긴다. |
| 긴급 수정 | `fix/<short-topic>` | 이미 `main`에 통합된 회귀만 최소 수정한다. |

Iteration 1(`iteration/1-tcp-http-response`)은 `main`에 병합 완료되었다. Iteration 2~6의 통합 브랜치는 갱신된 `main`에서 순서대로 분기해 진행한다.

## 체크포인트 산출물 및 리뷰 기준

매 Iteration 완료 시 각 개인 브랜치에서 다음을 제시한다.

1. 실행 가능한 코드와 빌드/테스트 명령(`make`, `make test`, `make verify`)
2. 해당 이슈(`#2`~`#6`)의 공통 완료 조건(DoD) 및 추가 반례 테스트 결과
3. 주요 설계 선택, 오류·자원 수명 처리, 알려진 한계
4. 후속 Iteration에서 구조를 바꿔야 할 가능성과 변경 비용

통합 브랜치에서는 모든 후보가 이슈의 DoD와 이전 Iteration 회귀 테스트를 충족하는지 먼저 확인한 후, 가장 단순하고 검증 근거가 명확한 구현을 기준으로 삼는다. 다른 후보의 개선은 회귀 테스트를 추가한 뒤에만 선택적으로 반영한다.
