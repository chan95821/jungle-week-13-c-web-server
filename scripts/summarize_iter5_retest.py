#!/usr/bin/env python3
"""측정 완료 JSON을 집계하고 증거와 Markdown을 보존한다. 새 요청은 보내지 않는다."""
import argparse
from collections import Counter, defaultdict
import csv
import hashlib
import json
from pathlib import Path
import shutil
from statistics import median

ROOT = Path(__file__).resolve().parents[1]
REPORT_DIRECTORY = ROOT / "docs/iteration-5/jaeseong"
SCENARIOS = {
    "baseline": "기존 조건 재측정",
    "high-load": "높은 조회 부하",
    "workers": "작업자 수 비교",
    "large-body": "큰 제출 본문",
}


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def span(values, digits=3):
    values = [value for value in values if value is not None]
    if not values:
        return "—"
    return f"{median(values):.{digits}f} ({min(values):.{digits}f}–{max(values):.{digits}f})"


def load_trials(directory):
    data = read_json(directory / "results.json")
    assert data["completed"], f"incomplete run: {directory}"
    assert len(data["trials"]) == data.get("expected_trials", 30), "missing trials"
    conditions = data["conditions"]
    rows = []
    for trial in data["trials"]:
        row = dict(trial)
        row.setdefault("scenario", "baseline")
        row.setdefault("workers", conditions.get("workers", 4))
        row.setdefault("submit_body_bytes", conditions.get("submit_body_bytes", 1024))
        row["source"] = directory.name
        for label, duration in (("warmup", conditions["warmup_s"]),
                                ("measured", conditions["measure_s"])):
            assert row[label]["duration_s"] == duration
            for kind, rate in (("submit", conditions["submit_rps"]), ("query", row["query_rps"])):
                value = row[label][kind]
                assert value["offered"] == round(rate * duration)
                assert value["success"] + value["failures"] == value["offered"]
                assert value["success"] <= value["started"] <= value["offered"]
                assert sum(value["backend_hits"].values()) == value["success"]
        rows.append(row)
    return rows


def group_trials(rows):
    groups = defaultdict(list)
    for row in rows:
        key = (row["scenario"], row["mode"], row["workers"], row["submit_body_bytes"], row["query_rps"])
        groups[key].append(row)
    for key, group in groups.items():
        assert sorted(row["repeat"] for row in group) == [1, 2, 3], key
    return groups


def export_evidence(directory):
    target = REPORT_DIRECTORY / "performance" / directory.name
    target.mkdir(parents=True, exist_ok=False)
    for source in directory.iterdir():
        if source.is_file() and source.suffix in (".json", ".log", ".patch", ".py"):
            shutil.copy2(source, target / source.name)
        elif source.is_dir() and (source / "result.json").is_file() and not source.name.startswith("smoke"):
            request_target = target / "requests" / source.name
            request_target.mkdir(parents=True)
            for evidence in source.iterdir():
                if evidence.name.endswith(("-requests.csv.gz", ".stderr.log")) or evidence.name == "result.json":
                    shutil.copy2(evidence, request_target / evidence.name)
    return target


def flattened_trial(row):
    result = {name: row[name] for name in ("source", "scenario", "mode", "workers", "submit_body_bytes", "query_rps", "repeat")}
    for phase in ("warmup", "measured"):
        result[f"{phase}_peak_inflight"] = row[phase]["peak_inflight"]
        for kind in ("submit", "query"):
            for name, value in row[phase][kind].items():
                if not isinstance(value, dict):
                    result[f"{phase}_{kind}_{name}"] = value
                else:
                    for category, count in value.items():
                        result[f"{phase}_{kind}_{name}_{category}"] = count
        for role, resource in row[phase]["resources"].items():
            for moment, values in resource.items():
                for metric, value in values.items():
                    result[f"{phase}_{role}_{moment}_{metric}"] = value
    return result


def totals(rows, phase):
    counts = Counter()
    categories = Counter()
    for row in rows:
        for kind in ("submit", "query"):
            value = row[phase][kind]
            for metric in ("offered", "started", "success", "failures", "timeout", "status_5xx", "generator_cap_rejections"):
                counts[metric] += value[metric]
            categories.update(value["failure_categories"])
    return {**counts, "failure_categories": dict(categories)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", nargs="+", type=Path, required=True)
    parser.add_argument("--report", type=Path, default=REPORT_DIRECTORY / "iteration-5-performance-retest-results.md")
    args = parser.parse_args()
    directories = [directory.resolve() for directory in args.input]
    report = args.report.resolve()
    assert not report.exists(), "refusing to overwrite an existing report"
    assert all(not (REPORT_DIRECTORY / "performance" / directory.name).exists() for directory in directories)
    rows = [row for directory in directories for row in load_trials(directory)]
    groups = group_trials(rows)
    scenarios = {row["scenario"] for row in rows}
    manifests = [read_json(directory / "manifest.json") for directory in directories]
    for directory, manifest in zip(directories, manifests):
        for relative, digest in manifest["snapshot_sources"].items():
            assert hashlib.sha256((directory / "snapshot" / relative).read_bytes()).hexdigest() == digest, relative
        assert hashlib.sha256((directory / "snapshot/build/server").read_bytes()).hexdigest() == manifest["server_sha256"]
        assert hashlib.sha256((directory / "snapshot/build/test_backend").read_bytes()).hexdigest() == manifest["backend_sha256"]
    assert all(item["snapshot_sources"] == manifests[0]["snapshot_sources"] for item in manifests), "service snapshots differ"
    assert all(item["server_sha256"] == manifests[0]["server_sha256"] for item in manifests), "service binaries differ"
    environments = [read_json(directory / "environment-at-run.json") for directory in directories]
    env = environments[0]
    measured, warmup = totals(rows, "measured"), totals(rows, "warmup")
    fd_changes = []
    for row in rows:
        for phase in ("warmup", "measured"):
            for role, resource in row[phase]["resources"].items():
                before, after = resource["before"]["fds"], resource["after"]["fds"]
                if before != after:
                    fd_changes.append({"source": row["source"], "scenario": row["scenario"], "mode": row["mode"],
                                       "workers": row["workers"], "query_rps": row["query_rps"], "repeat": row["repeat"],
                                       "phase": phase, "role": role, "before": before, "after": after})
    summary = {"trial_count": len(rows), "measured": measured, "warmup": warmup, "fd_changes": fd_changes}
    targets = [export_evidence(directory) for directory in directories]
    flat_rows = [flattened_trial(row) for row in rows]
    with (targets[0] / "trials.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=sorted(set().union(*(row.keys() for row in flat_rows))))
        writer.writeheader()
        writer.writerows(flat_rows)
    (targets[0] / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    title = "# B 현재 코드 성능 재측정 결과" if scenarios == {"baseline"} else "# B 현재 코드 성능 재측정·추가 측정 결과"
    lines = [title, "",
             "가독성 리팩토링 이후의 C 소스를 격리 빌드해 측정했다. 서버 구현·OS 한도는 바꾸지 않았다. 이전 결과는 별도로 보존한다.", "",
             f"- 완료 시행: {len(rows)}회. 측정 구간 예정 {measured['offered']:,}건, 성공 {measured['success']:,}건, 실패 {measured['failures']:,}건.",
             f"- 준비 구간: 예정 {warmup['offered']:,}건, 실패 {warmup['failures']:,}건. 측정 구간과 합쳐 숨기지 않는다.",
             f"- 측정 구간 실패 분류: `{measured['failure_categories']}`. 생성기 상한 거절 {measured['generator_cap_rejections']:,}건, 타임아웃 {measured['timeout']:,}건, HTTP 5xx {measured['status_5xx']:,}건.",
             f"- 준비·측정 구간에서 정리 후 FD 수가 시작과 달랐던 관측: {len(fd_changes)}개. 반복별 값은 원본에 보존했다.", "",
             "## 환경과 공통 조건", "",
             f"- 시작 UTC: {env['utc']}. CPU: {env['cpu_model']}, 논리 CPU {env['visible_logical_cpus']}개.",
             f"- Debian WSL2 / {env['kernel']} / {env['gcc']} / Python {env['python']}.",
             f"- WSL 메모리 {env['memory']['MemTotal_KiB'] / 1024 / 1024:.3f}GiB, FD soft/hard {env['fd_soft_hard']}.",
             "- 프록시 1, 동일 C 제출 백엔드 1 + 조회 백엔드 2. 메인 1 + 네트워크 작업자 N + 파일/CGI 보조 2. 직접 요청은 프록시를 거치지 않는다.",
             "- 제출 10건/초, 조회는 고정 13B 상태. 기본 POST는 1KiB 바이너리 echo." + (" 큰 본문 조건은 64KiB다." if "large-body" in scenarios else ""),
             "- 조건별 준비 3초 + 측정 15초, 3회 반복. 매 시행 새 서비스 프로세스. 요청당 새 TCP 연결 후 SHUT_WR·응답 EOF 검증.",
             "- Python 표준 asyncio open-loop 생성기. 예정 시각부터 제한 2초, 미완료 상한 256개. 상태·본문·백엔드 식별자가 모두 맞아야 성공이다.",
             "- GCC -std=c11 -Wall -Wextra -Werror -O2 -pthread, sanitizer 없음. 측정 중 다른 빌드·검증·성능 시행은 병행하지 않았다.",
             "- 모든 서비스와 생성기가 같은 WSL에서 실행됐다. 호스트의 다른 앱·클럭·전력은 통제하지 않았다.", "",
             "## 조건별 결과", "",
             "값은 3회 중앙값(최소–최대)이다. 제출 p95는 성공 요청의 예정 시각부터 응답 EOF·내용 검증까지이며 생성 지연을 포함한다. p95만 보고 실패 요청을 무시하지 않도록 실패율을 함께 표시한다.", "",
             "| 묶음 | 경로 | 작업자 | POST B | 조회 예정 req/s | 조회 성공 req/s | 제출 p95 ms | 제출 실패 % | 조회 실패 % |",
             "|---|---|---:|---:|---:|---:|---:|---:|---:|"]
    for key, values in sorted(groups.items()):
        scenario, mode, workers, size, rate = key
        phases = [row["measured"] for row in values]
        metrics = [span([phase[kind][metric] for phase in phases], digits)
                   for kind, metric, digits in (("query", "success_rps", 2), ("submit", "scheduled_p95_ms", 3),
                                                ("submit", "failure_pct", 2), ("query", "failure_pct", 2))]
        lines.append(f"| {SCENARIOS.get(scenario, scenario)} | {'B' if mode == 'b' else '직접'} | {workers if mode == 'b' else '—'} | {size} | {rate} | " + " | ".join(metrics) + " |")
    if "workers" in scenarios:
        lines += ["", "작업자 4개의 조회 800/POST 1KiB 기준은 기존 조건 재측정의 같은 결과를 재사용한다. 별도 중복 시행으로 표본 수를 늘리지 않는다."]
    lines += ["", "## 생성기 지연과 프록시 자원", "",
              "RSS/FD 최대는 1초 간격의 관측 최대다. 실제 순간 최대나 정확한 메모리 상한으로 해석하지 않는다. FD 전후 값은 각 단계 마지막 요청 정리 후 비교한다.", "",
              "| 묶음 | 작업자 | POST B | 조회 예정 req/s | 제출 실제 시작 기준 p95 ms | 제출 생성 지연 p95 ms | 프록시 RSS 관측 최대 MiB | FD 관측 최대 | 미완료 요청 관측 최대 |",
              "|---|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for key, values in sorted(groups.items()):
        scenario, mode, workers, size, rate = key
        if mode != "b":
            continue
        phases = [row["measured"] for row in values]
        metrics = [span([phase["submit"][name] for phase in phases]) for name in ("network_p95_ms", "dispatch_p95_ms")]
        rss = max(phase["resources"]["proxy"]["sampled_peak"]["rss_mib"] for phase in phases)
        fds = max(phase["resources"]["proxy"]["sampled_peak"]["fds"] for phase in phases)
        inflight = max(phase["peak_inflight"] for phase in phases)
        lines.append(f"| {SCENARIOS.get(scenario, scenario)} | {workers} | {size} | {rate} | {' | '.join(metrics)} | {rss:.3f} | {fds} | {inflight} |")
    lines += ["", "## 실패와 준비 구간", ""]
    details = []
    for row in rows:
        for phase in ("warmup", "measured"):
            for kind in ("submit", "query"):
                value = row[phase][kind]
                if value["failures"]:
                    details.append(f"- {row['scenario']}/{row['mode']}, 작업자 {row['workers']}, POST {row['submit_body_bytes']}B, 조회 {row['query_rps']}req/s, 반복 {row['repeat']}, {phase}/{kind}: {value['failures']}/{value['offered']}건, `{value['failure_categories']}`.")
    lines += details or ["준비·측정 구간 모두 실패가 없었다."]
    lines += ["", "## 해석 범위", "",
              "- 고정 요청률을 소화한 것은 해당 부하를 처리했다는 뜻이다. 포화점·최대 처리량·10,000연결을 측정했다고 표현하지 않는다.",
              "- 프록시/직접 비교도 순차 C 백엔드와 같은 WSL의 생성기 영향을 받는다. 생성기 상한 거절이나 생성 지연이 있으면 프록시의 실패·처리량으로만 원인을 단정할 수 없다.",
              "- 생성기 미완료 수는 클라이언트 연결 정리를 기다리는 작업도 포함한다. 프록시 내부의 실제 처리 중 요청 수와 같지 않다.",
              ("- 작업자 수 비교는 같은 B 모델 내부 비교다. A/C 모델, 동시성 도입 전후, 조회 서버 1개/2개의 효과를 검증한 결과는 아니다." if "workers" in scenarios else "- 네트워크 작업자 4개만 측정했다. 작업자 수 변화·큰 본문·A/C 모델·동시성 도입 전후·조회 서버 1개/2개 비교는 포함하지 않았다."),
              "- 제출 성공 표본은 시행당 최대 150개다. 실패율과 3회 범위를 함께 보고 작은 지연 차이를 우열로 단정하지 않는다.",
              "- 개인 참고 기준은 제출 p95 100ms·각 실패율 1%다. 팀 확정 채택 조건으로 간주하지 않는다. 결과에 따른 서버 최적화·설정 변경·허락하지 않은 추가 시행은 하지 않았다.",
              "- [이전 측정](iteration-5-performance-results.md)은 리팩토링 전 고정 스냅샷이다. 시점·호스트 상태가 다르므로 단순 차이를 리팩토링의 인과 효과로 주장하지 않는다.", "",
              "## 재현 자료", "",
              f"- 서버 SHA256: `{manifests[0]['server_sha256']}`.",
              f"- 전체 반복별 [CSV](performance/{directories[0].name}/trials.csv), [요약 JSON](performance/{directories[0].name}/summary.json)."]
    for directory in directories:
        lines.append(f"- `{directory.name}`: [전체 결과](performance/{directory.name}/results.json), [환경](performance/{directory.name}/environment-at-run.json), [해시](performance/{directory.name}/manifest.json). 요청별 압축 CSV와 stderr는 같은 폴더의 `requests/`에 보존했다. 실행 스냅샷은 `build/iteration5-performance/{directory.name}/snapshot/`이다.")
    lines += ["- 생성기와 실행 로그는 각 증거 폴더에 보존했다. `scripts/summarize_iter5_retest.py`는 완료 JSON의 오프라인 집계만 수행한다.", ""]
    report.write_text("\n".join(lines), encoding="utf-8")
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    print("Report:", report)


if __name__ == "__main__":
    main()
