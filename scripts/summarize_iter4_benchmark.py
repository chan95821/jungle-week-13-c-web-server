#!/usr/bin/env python3
"""Summarize already completed approved trials; never generates traffic."""
import csv
import json
import shutil
from pathlib import Path
from statistics import median

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build' / 'performance'
REPORT = ROOT / 'docs/iteration-4/Snoopykid/iteration-4-performance-results.md'

def main():
    data = json.loads((OUT / 'results.json').read_text())
    if not data.get('completed') or len(data['trials']) != 48:
        raise SystemExit('The full approved measurement has not completed; no final report written.')
    trials = data['trials']
    def rows(build, stage):
        selected = [r['measured'] for r in trials if r['build'] == build and r['stage'] == stage]
        assert len(selected) == 3
        return selected
    def span(values, decimals=3):
        return f'{median(values):.{decimals}f} ({min(values):.{decimals}f}–{max(values):.{decimals}f})'
    def stat(build, stage, role, key, decimals=3):
        return span([r[role][key] for r in rows(build, stage)], decimals)
    failures = sum(r['measured'][role]['failures'] for r in trials for role in ('submit', 'query'))
    warm_failures = sum(r['warmup'][role]['failures'] for r in trials for role in ('submit', 'query'))
    fives = sum(r['measured'][role]['status_5xx'] for r in trials for role in ('submit', 'query'))
    success = sum(r['measured'][role]['success'] for r in trials for role in ('submit', 'query'))
    deficits = []
    fd_changes = []
    for trial in trials:
        for role, rate in (('submit', 2), ('query', next(s[1] for s in data['conditions']['stages'] if s[0] == trial['stage']))):
            if rate:
                expected = rate * next(s[2] for s in data['conditions']['stages'] if s[0] == trial['stage'])
                if trial['measured'][role]['attempted'] != expected:
                    deficits.append([trial['build'], trial['stage'], trial['repeat'], role, expected, trial['measured'][role]['attempted']])
        for role, process in trial['measured']['processes'].items():
            if process['fds_before'] != process['fds_after']:
                fd_changes.append([trial['build'], trial['stage'], trial['repeat'], role,
                                   process['fds_before'], process['fds_after']])
    records = []
    for trial in trials:
        r = trial['measured']
        row = {k: trial[k] for k in ('build', 'stage', 'repeat')}
        for role in ('submit', 'query'):
            for key, value in r[role].items():
                if key != 'backend_hits': row[f'{role}_{key}'] = value
            for backend, count in r[role]['backend_hits'].items(): row[f'{role}_hits_{backend}'] = count
        for role, process in r['processes'].items():
            for key, value in process.items(): row[f'{role}_{key}'] = value
        row['resource_interval_s'] = r['resource_interval_s']
        row['orchestrator_cpu_s'] = r['orchestrator_cpu_s']
        records.append(row)
    fields = sorted(set().union(*(row.keys() for row in records)))
    with (OUT / 'trials.csv').open('w', newline='') as file:
        writer = csv.DictWriter(file, fieldnames=fields)
        writer.writeheader(); writer.writerows(records)
    before = [r['query']['success_rps'] for r in rows('baseline', 'S_MAX')]
    after = [r['query']['success_rps'] for r in rows('current', 'S_MAX')]
    change = (median(after) / median(before) - 1) * 100
    overlapping = max(min(before), min(after)) <= min(max(before), max(after))
    lines = [
        '# Iteration 4 보완 전후 성능 재측정', '',
        '2026-11-25 사용자 승인 조건으로 측정을 완료했다. 비교 대상은 f32e17d와 현재 작업 트리의 격리 스냅샷이다. 현재 코드는 미커밋 상태이며 바이너리/소스 SHA256은 build/performance/manifest.json에 기록했다.', '',
        '## 결과 요약', '',
        f'- 측정 48회 완료. 정상 검증된 응답 {success:,}건, 실패 {failures}건, HTTP 5xx {fives}건. 워밍업 실패 {warm_failures}건.',
        f'- 포화 조회 처리량 중앙값: 보완 전 {median(before):,.1f} → 보완 후 {median(after):,.1f} req/s ({change:+.2f}%).',
        '- 3회 최소~최대 범위가 ' + ('겹친다. 이 결과만으로 안정적인 개선/저하를 단정하지 않는다.' if overlapping else '겹치지 않는다. 관측 차이는 기록하되 추가 조건이나 반복 없이 일반적인 성능 보장으로 확대하지 않는다.'),
        f'- 고정 요청률의 예정 건수 미달 사례: {len(deficits)}개. 측정 전후 FD 수 차이: {len(fd_changes)}개.', '',
        '## 측정 조건', '',
        '- Ryzen 7 4800H(8코어/16논리 CPU), Ubuntu WSL2, WSL 메모리 약 7.5GiB. 호스트 16GB는 사용자 제공 정보와 구분한다.',
        '- 공통 C 백엔드 제출1/조회2, 각 응답 1024바이트·인위 지연0. 제출은 1024바이트 바이너리 echo, 조회는 동일한 바이트 패턴이다. 기존 C 백엔드의 benchmark 모드를 두 후보에 똑같이 사용했다.',
        '- C11 -Wall -Wextra -Werror -O2, sanitizer 없는 측정 바이너리. C 부하 생성기, Python은 실행/자원 관찰/집계만 담당한다.',
        '- 요청마다 새 TCP 연결, 클라이언트 요청 완료 후 SHUT_WR, 응답은 EOF까지 수신. 전체 요청 제한시간5초. 상태/Content-Length/1024바이트 내용/백엔드ID가 맞아야 성공이다.',
        '- 제출2req/s(최대 동시2), 조회10/25/50/100/200/400req/s(최대 동시16). S_MAX 조회는 동시16 closed-loop이며 제출2req/s를 병행한다.',
        '- 각 조건 워밍업5초 후3회. S1 측정60초, S2~S6 각8초, S_MAX 5초. 직접 백엔드 기준선은 S1/S5이며 조회 A/B를 클라이언트에서 교대로 선택한다.',
        '- 후보당 프록시는 매 회 새로 시작한다. 3개 백엔드는 공통 세션에서 유지한다. 단계별 후보 실행 순서는 반복1/3 보완전→후, 반복2 보완후→전이다.',
        '- 지연은 클라이언트 socket 생성 직전부터 응답 EOF·바이트 검증·close까지다. 단위ms, 성공 응답의 nearest-rank p50/p95만 집계한다. 표는 반복별 값의 중앙값(최소–최대)이며 모든 응답을 합친 전역 p95가 아니다.',
        '- 처리량은 성공 건수/부하 생성기의 실제 경과시간(마지막 요청 정리 포함)이다. 고정률 도구는 동시수 상한이 있어 무한 큐를 만들지 않는다. 예정 건수 미달과 실패를 별도로 기록한다.', '',
        '## 단계별 보완 전후', '',
        '| 단계 | 보완 전 조회 req/s | 보완 후 조회 req/s | 전 조회 p95 ms | 후 조회 p95 ms | 전 제출 p95 ms | 후 제출 p95 ms |',
        '| --- | --- | --- | --- | --- | --- | --- |'
    ]
    for stage, _, _ in data['conditions']['stages']:
        cells = [stage, stat('baseline', stage, 'query', 'success_rps', 1), stat('current', stage, 'query', 'success_rps', 1),
                 stat('baseline', stage, 'query', 'p95_ms'), stat('current', stage, 'query', 'p95_ms'),
                 stat('baseline', stage, 'submit', 'p95_ms'), stat('current', stage, 'submit', 'p95_ms')]
        lines.append('| ' + ' | '.join(cells) + ' |')
    lines += ['', '## p50 지연', '', '| 단계 | 전 제출 p50 ms | 후 제출 p50 ms | 전 조회 p50 ms | 후 조회 p50 ms |', '| --- | --- | --- | --- | --- |']
    for stage, _, _ in data['conditions']['stages']:
        lines.append('| ' + ' | '.join([stage, stat('baseline', stage, 'submit', 'p50_ms'),
            stat('current', stage, 'submit', 'p50_ms'), stat('baseline', stage, 'query', 'p50_ms'),
            stat('current', stage, 'query', 'p50_ms')]) + ' |')
    lines += ['', '## S1 품질 상한', '', '| 후보 | 제출 p95 중앙값/최악 반복 ms | 제출 실패 | 조회 실패/시도 | 5xx | 판정 |', '| --- | --- | --- | --- | --- | --- |']
    gates = {}
    for build in ('baseline', 'current'):
        group = rows(build, 'S1')
        p95 = [r['submit']['p95_ms'] for r in group]
        sf = sum(r['submit']['failures'] for r in group)
        qf = sum(r['query']['failures'] for r in group)
        qa = sum(r['query']['attempted'] for r in group)
        ff = sum(r[role]['status_5xx'] for r in group for role in ('submit', 'query'))
        gate = (max(p95) <= 500 and sf == 0 and qf / max(qa, 1) <= .01 and ff == 0
                and all(r["submit"]["attempted"] == 120 and r["query"]["attempted"] == 600 for r in group))
        gates[build] = gate
        lines.append(f'| {build} | {median(p95):.3f} / {max(p95):.3f} | {sf} | {qf}/{qa} | {ff} | {"통과" if gate else "실패"} |')
    lines += ['', '## 직접 백엔드 기준선과 추가 지연', '', '| 조건 | 직접 제출 p95 ms | 직접 조회 p95 ms | 전 제출/조회 추가 p95 ms | 후 제출/조회 추가 p95 ms |', '| --- | --- | --- | --- | --- |']
    for stage in ('S1', 'S5'):
        direct_s = median(r['submit']['p95_ms'] for r in rows('direct', stage))
        direct_q = median(r['query']['p95_ms'] for r in rows('direct', stage))
        extra = []
        for build in ('baseline', 'current'):
            extra.append('/'.join(f'{median(r[role]["p95_ms"] for r in rows(build, stage)) - direct:.3f}'
                                  for role, direct in (('submit', direct_s), ('query', direct_q))))
        lines.append(f'| {stage} | {stat("direct", stage, "submit", "p95_ms")} | {stat("direct", stage, "query", "p95_ms")} | {extra[0]} | {extra[1]} |')
    lines += ['', '추가 지연은 서로 다른 실행의 p95 중앙값 차이이며 요청별 프록시 내부 시간의 직접 측정이 아니다. 음수는 측정 변동의 영향을 포함한다.', '',
              '## S_MAX 자원 사용', '', '| 후보 | 프로세스 | CPU 초 user+system | CPU % (100%=논리CPU1개) | RSS 표본 최대 KiB |', '| --- | --- | --- | --- | --- |']
    for build in ('baseline', 'current'):
        group = rows(build, 'S_MAX')
        for role in ('proxy', 'submission', 'query-a', 'query-b'):
            cpu = [r['processes'][role]['cpu_user_s'] + r['processes'][role]['cpu_system_s'] for r in group]
            percent = [100 * c / r['resource_interval_s'] for c, r in zip(cpu, group)]
            rss = [r['processes'][role]['rss_sampled_peak_kib'] for r in group]
            lines.append(f'| {build} | {role} | {span(cpu)} | {span(percent, 1)} | {span(rss, 0)} |')
        for role in ('submit', 'query'):
            cpu = [r[role]['cpu_user_s'] + r[role]['cpu_system_s'] for r in group]
            percent = [100 * c / r[role]['duration_s'] for c, r in zip(cpu, group)]
            rss = [r[role]['max_rss_kib'] for r in group]
            lines.append(f'| {build} | C loader {role} | {span(cpu)} | {span(percent, 1)} | {span(rss, 0)} |')
    lines += ['', '프록시/백엔드 CPU는 /proc stat의 tick 차이, RSS는 100ms 간격 표본 최대다. 짧은 메모리 피크를 놓칠 수 있다. C 부하 생성기 CPU와 최대RSS는 getrusage(RUSAGE_SELF)다. 최대RSS는 실행 시작 이력을 포함하는 프로세스 수명 고수위이며 순수 C 버퍼 사용량이나 같은 시각의 상주량으로 해석하지 않는다. 제출 백엔드 CPU의 0.000은 tick 해상도 아래일 수 있어 비용이 없다는 뜻이 아니다. warmup 뒤 RSS를 전체 연결별 버퍼 사용량으로 해석하지 않는다.', '',
              '## A/B 분산과 정리', '', '| 후보 | 측정 조회 A | 측정 조회 B | 프록시 FD 전후 집합 |', '| --- | --- | --- | --- |']
    for build in ('baseline', 'current'):
        subset = [r['measured'] for r in trials if r['build'] == build]
        counts = [sum(r['query']['backend_hits'][role] for r in subset) for role in ('query-a', 'query-b')]
        pairs = sorted({(r['processes']['proxy']['fds_before'], r['processes']['proxy']['fds_after']) for r in subset})
        lines.append(f'| {build} | {counts[0]} | {counts[1]} | {pairs} |')
    lines += ['', '## 한계 및 해석', '',
              '- 이 비교는 f32e17d 이후 HTTP/1.0·보호 헤더·헤더 정책·모듈/메모리·라우팅 보완 전체 묶음의 효과다. 메모리 리팩토링만의 인과 효과로 분리하지 않았다.',
              '- 기존 핸드오프는 다른 Linux/Go 백엔드·부하 도구였다. 이번 C/WSL 결과로 기존 4,937req/s 유지 여부를 판정하거나 외부 후보 순위를 다시 매기지 않는다.',
              '- 같은 노트북에서 프록시·백엔드·부하 생성기가 CPU/메모리를 경쟁한다. 부하 생성기 병목을 완전히 배제하지 못했다. CPU 고정·네트워크 sysctl·FD 한도 변경은 하지 않았다.',
              '- 정상 1KiB 응답 조건의 결과다. 느린/장애 백엔드·큰 본문·1만 연결·장시간 메모리 증가는 측정하지 않았다. 순차 처리의 head-of-line blocking이 해결된 것은 아니다.',
              '- S2~S6의 제출은 반복당 약16건, S_MAX는 약10건이므로 제출 p95 표본이 작다. 관측 변동을 포함하며 개선 보장으로 확대하지 않는다.',
              '- 최적화·정책 변경·추가 측정을 자동 적용하지 않았다. 머지·커밋·푸시는 하지 않았다.', '',
              '## 원시 자료', '',
              '- build/performance/results.json: 워밍업/측정, 환경, 과정별 수치 전체.',
              '- build/performance/trials.csv: 측정48회 지연·처리량·CPU/RSS·FD 평탄화 자료.',
              '- build/performance/manifest.json: 스냅샷/바이너리/도구 SHA256.',
              '- build/performance/run.log 및 build.log: 실행 순서와 빌드 기록.',
              '- scripts/benchmark_iter4.py, tests/performance/load_client.c: 재현 도구. prepare → smoke → run → scripts/summarize_iter4_benchmark.py 순서다. prepare는 기존 스냅샷, run은 기존 결과가 있으면 덮어쓰지 않고 종료한다. 새 실행은 기존 증거를 보존한 별도 출력 디렉터리 설계/조건 합의 후 진행한다. 이번 실행 스크립트 원문과 SHA256도 별도로 보존했다.', '',
              f'예정 건수 미달 상세: {deficits}', f'FD 차이 상세: {fd_changes}', '']
    REPORT.write_text('\n'.join(lines))
    summary = {'completed_trials': len(trials), 'successes': success, 'failures': failures,
               'status_5xx': fives, 'warmup_failures': warm_failures,
               'baseline_smax_median_rps': median(before), 'current_smax_median_rps': median(after),
               'smax_change_pct': change, 'ranges_overlap': overlapping, 's1_gate': gates,
               'offered_count_deficits': deficits, 'fd_changes': fd_changes}
    (OUT / 'summary.json').write_text(json.dumps(summary, indent=2))
    archive = REPORT.parent / 'performance'
    archive.mkdir(exist_ok=True)
    for name in ('results.json', 'trials.csv', 'manifest.json', 'summary.json'):
        shutil.copy2(OUT / name, archive / name)
    with REPORT.open('a') as report:
        report.write('\nGit에 보존할 텍스트 자료: [전체 원시 결과](performance/results.json), '
                     '[48회 CSV](performance/trials.csv), [소스/바이너리 지문](performance/manifest.json), '
                     '[요약](performance/summary.json). 실행 바이너리와 로그는 build/에 유지한다.\n')
    print(json.dumps(summary, indent=2))
    print(REPORT)

if __name__ == '__main__': main()