"""Create a portable SVG from recorded wrk results; does not run a benchmark."""
import json
import re
import statistics
from pathlib import Path
from html import escape

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'docs/iteration-5/status-benchmark-20261126/results.json'
TARGETS = [('jaeseong', '스레드별 epoll', '#1264d6'),
           ('chan', 'Blocking 스레드 풀', '#d33d50'),
           ('yyj_after', '프로세스별 epoll', '#15957c')]


def summarize():
    records = json.loads(SOURCE.read_text(encoding='utf-8'))['results']
    output = {}
    for target, _, _ in TARGETS:
        for mode in ('submit', 'query'):
            rows = [r for r in records if r['target'] == target
                    and r['mode'] == mode and r['connections'] == 1000]
            assert len(rows) == 3, (target, mode, len(rows))
            rates, p95 = [], []
            for row in rows:
                text = row['stdout']
                valid = int(re.search(r'CLASS valid (\d+)', text)[1])
                seconds = float(re.search(r'\d+ requests in ([\d.]+)s', text)[1])
                rates.append(valid / seconds)
                p95.append(float(re.search(r'PERCENTILE_US p50=\S+ p95=([\d.]+)', text)[1]) / 1000)
            output[target, mode] = (statistics.median(rates), statistics.median(p95))
    return output


def main():
    data = summarize()
    svg = ['<svg xmlns="http://www.w3.org/2000/svg" width="1120" height="410" viewBox="0 0 1120 410" role="img" aria-labelledby="title desc">',
           '<title id="title">동시 연결 1,000개: 세 동시성 모델 성공 처리량 비교</title>',
           '<desc id="desc">제출과 조회 각각 30초 세 번 측정한 성공 처리량 중앙값.</desc>',
           '<style>text{font-family:Malgun Gothic,Apple SD Gothic Neo,sans-serif;fill:#172b40}.small{font-size:13px}.label{font-size:14px}.head{font-size:19px;font-weight:700}</style>',
           '<rect x="1" y="1" width="1118" height="408" rx="14" fill="white" stroke="#d2dbe5"/>',
           '<text x="28" y="36" class="head">동시 연결 1,000개 · 제출과 조회 분리 측정</text>',
           '<text x="28" y="61" class="small">wrk · 작업자 4개 · 요청마다 새 연결 · 각 30초 × 3회 중앙값</text>']
    for panel, (metric, title, maximum, ticks, unit) in enumerate([
        (0, '성공 처리량 (req/s) — 높을수록 좋음', 20000, [0, 5000, 10000, 15000, 20000], '')]):
        top = 103 + panel * 288
        svg.append(f'<text x="28" y="{top}" class="head">{title}</text>')
        left, width = 260, 790
        for tick in ticks:
            x = left + width * tick / maximum
            svg.append(f'<path d="M{x},{top+17} V{top+174}" stroke="#e6ebf1"/><text x="{x}" y="{top+197}" text-anchor="middle" class="small">{tick:,}</text>')
        for index, (target, label, color) in enumerate(TARGETS):
            y = top + 26 + index * 49
            svg.append(f'<text x="245" y="{y+21}" text-anchor="end" class="label">{escape(label)}</text>')
            for j, mode in enumerate(('submit', 'query')):
                value = data[target, mode][metric]
                length = width * value / maximum
                opacity = '1' if j == 0 else '.45'
                svg.append(f'<rect x="{left}" y="{y+j*19}" width="{length}" height="15" rx="3" fill="{color}" opacity="{opacity}"/>')
                svg.append(f'<text x="{left+length+7}" y="{y+j*19+12}" class="small">{value:,.0f}' + ('</text>' if metric == 0 else f' ms</text>'))
    svg.extend(['<text x="28" y="344" class="label">각 쌍: 위쪽 진한 막대 = 제출 · 아래쪽 연한 막대 = 조회</text>',
                '<text x="28" y="378" class="small">정상 응답만 집계한 처리량. 오류 응답·안정성 및 측정 조건의 한계는 본문 참조.</text>', '</svg>'])
    path = ROOT / 'docs/assets/iteration5-concurrency-comparison.svg'
    path.write_text('\n'.join(svg), encoding='utf-8')
    print(json.dumps({f'{t}/{m}': v for (t, m), v in data.items()}, indent=2))


if __name__ == '__main__':
    main()
