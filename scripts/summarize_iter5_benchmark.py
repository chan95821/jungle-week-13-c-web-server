#!/usr/bin/env python3
"""Offline summary only: no service processes or new traffic are started here."""
import argparse
import csv
import json
from pathlib import Path
import shutil
from statistics import median

ROOT = Path(__file__).resolve().parents[1]
RATES = (50, 100, 200, 400, 800)


def format_span(values, places=3):
    return f"{median(values):.{places}f} ({min(values):.{places}f}–{max(values):.{places}f})"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    options = parser.parse_args()
    directory = options.input.resolve()
    data = json.loads((directory / "results.json").read_text())
    environment = json.loads((directory / "environment-at-run.json").read_text())
    manifest = json.loads((directory / "manifest.json").read_text())
    trials = data["trials"]
    assert data["completed"] and len(trials) == 30, "approved measurement is incomplete"
    for mode in ("b", "direct"):
        for rate in RATES:
            selected = [row for row in trials if row["mode"] == mode and row["query_rps"] == rate]
            assert sorted(row["repeat"] for row in selected) == [1, 2, 3]
    for trial in trials:
        for label, duration in (("warmup", 3), ("measured", 15)):
            for kind, rate in (("submit", 10), ("query", trial["query_rps"])):
                value = trial[label][kind]
                assert value["offered"] == rate * duration
                assert value["success"] + value["failures"] == value["offered"]
                assert value["success"] <= value["started"] <= value["offered"]
                assert sum(value["backend_hits"].values()) == value["success"]
    artifacts = ROOT / "docs/iteration-5/jaeseong/performance" / directory.name
    artifacts.mkdir(parents=True, exist_ok=False)
    for name in ("manifest.json", "environment.json", "environment-at-run.json", "results.json", "build.log", "worktree.patch"):
        source = directory / name
        if source.exists():
            shutil.copy2(source, artifacts / name)
    for trial_dir in directory.iterdir():
        if trial_dir.is_dir() and (trial_dir.name.startswith("b-q") or trial_dir.name.startswith("direct-q")):
            target = artifacts / "requests" / trial_dir.name
            target.mkdir(parents=True)
            for source in trial_dir.glob("*-requests.csv.gz"):
                shutil.copy2(source, target / source.name)

    rows = []
    for trial in trials:
        row = {key: trial[key] for key in ("mode", "query_rps", "repeat")}
        phase = trial["measured"]
        row["peak_inflight"] = phase["peak_inflight"]
        for kind in ("submit", "query"):
            row.update({f"{kind}_{key}": value for key, value in phase[kind].items()
                        if not isinstance(value, dict)})
            for backend, count in phase[kind]["backend_hits"].items():
                row[f"{kind}_hits_{backend}"] = count
        for role, value in phase["resources"].items():
            for moment in ("before", "sampled_peak", "after"):
                for metric, number in value[moment].items():
                    row[f"{role}_{moment}_{metric}"] = number
        rows.append(row)
    with (artifacts / "trials.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=sorted(set().union(*(row.keys() for row in rows))))
        writer.writeheader()
        writer.writerows(rows)

    def measures(mode, rate):
        return [row["measured"] for row in trials if row["mode"] == mode and row["query_rps"] == rate]

    def span(mode, rate, kind, key, places=3):
        return format_span([row[kind][key] for row in measures(mode, rate)], places)

    successful = sum(trial["measured"][kind]["success"] for trial in trials for kind in ("submit", "query"))
    failed = sum(trial["measured"][kind]["failures"] for trial in trials for kind in ("submit", "query"))
    warm_failed = sum(trial["warmup"][kind]["failures"] for trial in trials for kind in ("submit", "query"))
    warm_cap = sum(trial["warmup"][kind]["generator_cap_rejections"] for trial in trials for kind in ("submit", "query"))
    warm_details = []
    for trial in trials:
        for kind in ("submit", "query"):
            value = trial["warmup"][kind]
            if value["failures"]:
                warm_details.append(
                    f"- {trial['mode']} / 조회 {trial['query_rps']}req/s / 반복 {trial['repeat']} / {kind}: "
                    f"{value['failures']}/{value['offered']}건 실패, 분류 {value['failure_categories']}.")
    cap_rejected = sum(trial["measured"][kind]["generator_cap_rejections"] for trial in trials for kind in ("submit", "query"))
    fd_changes = []
    for trial in trials:
        for role, value in trial["measured"]["resources"].items():
            if value["after"]["fds"] != value["before"]["fds"]:
                fd_changes.append({"mode": trial["mode"], "rate": trial["query_rps"], "repeat": trial["repeat"],
                                   "role": role, "before": value["before"]["fds"], "after": value["after"]["fds"]})
    b_trials = [row["measured"] for row in trials if row["mode"] == "b"]
    highest = measures("b", 800)
    summary = {"successful": successful, "failed": failed, "warmup_failed": warm_failed,
               "warmup_cap_rejections": warm_cap,
               "generator_cap_rejections": cap_rejected, "fd_changes": fd_changes,
               "b_q800_query_rps_median": median(row["query"]["success_rps"] for row in highest),
               "b_q800_submit_p95_ms_median": median(row["submit"]["scheduled_p95_ms"] for row in highest),
               "b_peak_proxy_rss_mib": max(row["resources"]["proxy"]["sampled_peak"]["rss_mib"] for row in b_trials),
               "b_peak_proxy_fds": max(row["resources"]["proxy"]["sampled_peak"]["fds"] for row in b_trials),
               "b_peak_inflight": max(row["peak_inflight"] for row in b_trials)}
    (artifacts / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    lines = ["# Iteration 5 B 성능 측정 결과", "",
             "2026-11-25 사용자가 승인한 조건으로 격리 스냅샷의 30회 측정을 완료했다. 팀 후보 채택/다른 동시성 모델과의 우열 비교 결과가 아니다.", "",
             "## 관측 결과", "",
             f"- 측정 요청 {successful + failed:,}건 중 바이트·상태·백엔드 ID 검증 성공 {successful:,}건, 실패 {failed}건. 준비 구간 실패 {warm_failed}건.",
             f"- 조회 800req/s 조건의 B 중앙값: 조회 성공 {summary['b_q800_query_rps_median']:.2f}req/s, 제출 p95 {summary['b_q800_submit_p95_ms_median']:.3f}ms.",
             f"- B의 샘플링된 프록시 RSS 최대 {summary['b_peak_proxy_rss_mib']:.3f}MiB, 열린 FD 최대 {summary['b_peak_proxy_fds']}개. 측정 전후 FD 변화 사례 {len(fd_changes)}개.",
             f"- 생성기 256개 상한 거절 {cap_rejected}건. B 미완료 요청 관측 최대 {summary['b_peak_inflight']}개.",
             f"- 준비 구간은 별도 집계했으며 미완료 상한으로 전송 전 거절된 요청이 {warm_cap}건 있다. 측정 구간 실패 0건을 전체 실행 실패 0건으로 확대하지 않는다.",
             "- 최고 승인 부하까지 수용했다. 포화에 도달했는지는 확인하지 않았으므로 최대 처리량이나 10,000연결 지원으로 해석할 수 없다.", "",
             "## 조건", "",
             f"- CPU: {environment['cpu_model']}, WSL에 보이는 논리 CPU {environment['visible_logical_cpus']}개, 메모리 {environment['memory']['MemTotal_KiB'] / 1024 / 1024:.3f}GiB.",
             f"- Debian 13 WSL2 / {environment['kernel']} / {environment['gcc']} / Python {environment['python']}.",
             f"- FD soft/hard limit {environment['fd_soft_hard']}; OS 제한·TCP 설정·CPU affinity를 바꾸지 않았다.",
             "- C 프록시 1개(네트워크 4 + 보조 2), 동일 C 백엔드 제출 1 + 조회 2. 각 백엔드는 순차 처리하는 normal 모드. 직접 요청도 같은 백엔드 코드·작업·응답을 사용했다.",
             "- 제출: POST /api/submissions, 1024B 바이너리 echo, 10req/s 고정. 조회: GET /api/submission-status, 고정 13B 바이너리 상태, 50/100/200/400/800req/s.",
             "- 새 TCP 연결마다 한 요청, Connection close, 요청 전송 후 클라이언트 SHUT_WR. 요청 기한은 예정 시각부터 2초.",
             "- 조건별 준비 3초 + 측정 15초를 3회. B/직접 순서를 반복별로 교차했고, 매 시행 서비스 프로세스를 새로 시작했다. 모든 생성기·서비스는 같은 WSL에서 실행했다.",
             "- GCC -std=c11 -Wall -Wextra -Werror -O2, 프록시 -pthread, sanitizer 없음. 측정 중 다른 빌드·기능 테스트·튜닝은 실행하지 않았다.",
             "- 측정 중 작업 소스의 한글 주석 편집은 병행했지만 실행 중인 격리 소스/바이너리는 고정했다. 호스트의 다른 앱·전력·클럭을 통제한 실험은 아니다.", "",
             "## 성공 처리량과 제출 지연", "",
             "각 칸은 3회 중앙값(최소–최대)이다. p95는 성공 요청의 예정 시각부터 EOF·내용 검증까지의 nearest-rank다. 실패율은 예정 요청 전체가 분모다.", "",
             "| 조회 예정 req/s | B 조회 성공 req/s | 직접 조회 성공 req/s | B 제출 p95 ms | 직접 제출 p95 ms | B 제출/조회 실패율 % |",
             "|---:|---:|---:|---:|---:|---:|"]
    for rate in RATES:
        values = measures("b", rate)
        lines.append("| " + " | ".join((str(rate), span("b", rate, "query", "success_rps", 2),
            span("direct", rate, "query", "success_rps", 2), span("b", rate, "submit", "scheduled_p95_ms"),
            span("direct", rate, "submit", "scheduled_p95_ms"),
            f"{median(row['submit']['failure_pct'] for row in values):.2f} / {median(row['query']['failure_pct'] for row in values):.2f}")) + " |")
    lines += ["", "## 준비 구간의 별도 실패", "",
              *(warm_details or ["준비 구간 실패가 없었다."]), "",
              f"준비 구간 실패 {warm_failed}건 중 생성기 상한 거절은 {warm_cap}건이다. 15초 측정 구간과 분리해 보존한다. 초기 응답 지연·요청 생성 스케줄링·호스트 정지 중 무엇이 상한 도달을 유발했는지는 이번 데이터만으로 확정하지 않는다. 조건을 바꾼 재측정은 하지 않았다.", "",
              "## 부하 생성기 지연을 분리한 참고값", "",
              "예정 시각 기준 주 지연에는 Python 스케줄링 지연이 포함된다. 다음 실제 시작 기준 값과 함께 확인해야 프록시 자체 지연으로 과대 해석하지 않는다.", "",
              "| 조회 예정 req/s | B 제출 실제 시작 기준 p95 ms | B 제출 생성 지연 p95 ms | B 조회 실제 시작 기준 p95 ms |",
              "|---:|---:|---:|---:|"]
    for rate in RATES:
        lines.append(f"| {rate} | {span('b', rate, 'submit', 'network_p95_ms')} | "
                     f"{span('b', rate, 'submit', 'dispatch_p95_ms')} | {span('b', rate, 'query', 'network_p95_ms')} |")
    lines += ["", "## 자원과 해석 범위", "",
              "RSS/FD 최대는 1초 간격 관측값이며 절대 최대를 보장하지 않는다. 전후 FD는 마지막 요청 정리 0.3초 뒤 비교했다.", ""]
    if fd_changes:
        lines += ["FD 변화 원본 사례:", "", "```json", json.dumps(fd_changes, ensure_ascii=False, indent=2), "```", ""]
    else:
        lines += ["모든 시행의 모든 관측 프로세스에서 측정 전후 FD 수가 같았다.", ""]
    all_valid = all(row["submit"]["scheduled_p95_ms"] <= 100 and row["submit"]["failure_pct"] <= 1 and
                    row["query"]["failure_pct"] <= 1 for row in b_trials)
    lines += [f"개인 잠정 상한(제출 p95 100ms, 제출/조회 실패율 각각 1%)은 B의 모든 시행에서 {'충족했다' if all_valid else '일부 미충족했다'}. 이 값은 팀 확정 상한으로 간주하지 않는다.", "",
              "- 요청률을 정해 둔 open-loop 실험이라 처리량이 예정값과 같다는 것은 그 부하를 소화했다는 뜻이다. 800보다 높은 부하/포화점은 측정하지 않았다.",
              "- B와 직접 요청은 같은 부하 구간의 참고 비교다. 동시성 전후, A/C 모델, 조회 1개 vs 2개 비교는 하지 않았다.",
              "- 과거 Iteration 4 보고서는 CPU·OS·응답 크기·요청률·도구가 다르므로 이번 숫자와 성능 향상률을 계산하지 않는다.",
              "- 순차 C 백엔드와 같은 WSL의 Python 생성기가 상한을 제한할 수 있다. 15초 측정 구간에서는 상한 거절/실패가 없었지만, 준비 구간에는 위 상한 거절이 있었고 포화 원인을 판정한 실험은 아니다.",
              "- 제출 성공 표본은 시행당 150개다. 3회 변동 범위를 보존하며 작은 지연 차이를 우열로 단정하지 않는다.",
              "- 결과를 이유로 서버를 최적화하거나 조건/반복을 추가하지 않았다.", "",
              "## 재현 증거", "",
              f"- 시작 revision: `{manifest['revision_at_start']}` + 작업 트리의 주석 차이. 실제 소스 SHA256과 바이너리 SHA256은 manifest에 있다.",
              f"- 서버 SHA256: `{manifest['server_sha256']}`.",
              f"- 실행 스냅샷: `build/iteration5-performance/{directory.name}/snapshot/`.",
              f"- [반복별 CSV](performance/{directory.name}/trials.csv), [전체 결과 JSON](performance/{directory.name}/results.json), "
              f"[요약 JSON](performance/{directory.name}/summary.json), [환경](performance/{directory.name}/environment-at-run.json), "
              f"[해시](performance/{directory.name}/manifest.json).",
              f"- 요청별 예정/시작/완료 시각과 결과는 `performance/{directory.name}/requests/`의 압축 CSV에 보존했다. 준비·측정 요청은 별도 파일이다.",
              "- 생성기: scripts/benchmark_iter5.py. 보고서 집계: scripts/summarize_iter5_benchmark.py. 집계는 새 요청을 발생시키지 않는다.", ""]
    report = ROOT / "docs/iteration-5/jaeseong/iteration-5-performance-results.md"
    if report.exists():
        raise RuntimeError("report already exists; refusing to overwrite prior evidence")
    report.write_text("\n".join(lines), encoding="utf-8")
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    print("Report:", report)


if __name__ == "__main__":
    main()
