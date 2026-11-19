# 방당 CPU 원인 분해 집계: raw/game-load-<tag>.csv와 서버 로그(raw/game-server-<label>.log.gz)를 합쳐
# 조건별 서버 CPU와 틱 단계별 시간(측정 구간의 증가분)을 중앙값(최소~최대)으로 출력한다.
# 사용: python analyze_decompose.py [tag]   (기본 decomp)
import csv, gzip, os, re, statistics as st, sys, collections

tag = sys.argv[1] if len(sys.argv) > 1 else 'decomp'
raw = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'raw')
rows = list(csv.DictReader(open(os.path.join(raw, 'game-load-%s.csv' % tag), encoding='utf-8-sig')))


def to_s(t):
    h, m, s = t.split(':')
    return int(h) * 3600 + int(m) * 60 + float(s)


stat_re = re.compile(
    r'^(\d\d:\d\d:\d\d\.\d\d\d) \[INFO \] \[통계\].*게임\{.*?틱=(\d+) 건너뜀=(\d+).*?'
    r'틱시간\{이동=(\d+) 갱신=(\d+) 구성=(\d+) 전송=(\d+) 합=(\d+)\}')


def window(label, start, end):
    lines = []
    for l in gzip.open(os.path.join(raw, 'game-server-%s.log.gz' % label), 'rt', encoding='utf-8-sig', errors='replace'):
        m = stat_re.match(l)
        if m:
            lines.append((to_s(m.group(1)),) + tuple(int(m.group(i)) for i in range(2, 9)))
    s, e = to_s(start), to_s(end)
    before = [x for x in lines if x[0] <= s]
    after = [x for x in lines if x[0] <= e]
    if not before or not after:
        return None
    b, a = before[-1], after[-1]
    secs = a[0] - b[0]
    ticks = a[1] - b[1]
    if secs <= 0 or ticks <= 0:
        return None
    # 인덱스: 0 시각, 1 틱, 2 건너뜀, 3 이동, 4 갱신, 5 구성, 6 전송, 7 합 (마이크로초 누적)
    return dict(secs=secs, ticks=ticks, skipped=a[2] - b[2],
                per_tick=[(a[i] - b[i]) / ticks for i in range(3, 8)],
                ms_per_s=[(a[i] - b[i]) / secs / 1000.0 for i in range(3, 8)])


groups = collections.OrderedDict()
for r in rows:
    cond = r['label'].split('-')[1]
    w = window(r['label'], r['measure_start'], r['measure_end'])
    groups.setdefault(cond, []).append((r, w))


def med(vals, d=1):
    vals = [v for v in vals if v is not None]
    if not vals:
        return 'n/a'
    return '%.*f (%.*f~%.*f)' % (d, st.median(vals), d, min(vals), d, max(vals))


print('| 조건 | 판정 | 서버 CPU % | 틱 합 ms/s | 이동 | 갱신 | 구성 | 전송 | 합 밖 ms/s (CPU - 틱) |')
print('| --- | --- | --- | --- | --- | --- | --- | --- | --- |')
for cond, items in groups.items():
    cpu = [float(r['server_cpu_pct']) for r, _ in items]
    ws = [w for _, w in items if w]
    passes = sum(r['verdict'] == 'pass' for r, _ in items)
    outside = [float(r['server_cpu_pct']) * 10.0 - w['ms_per_s'][4] for r, w in items if w]  # CPU% x 10 = ms/s
    print('| %s | %d/%d | %s | %s | %s | %s | %s | %s | %s |' % (
        cond, passes, len(items), med(cpu),
        med([w['ms_per_s'][4] for w in ws]),
        med([w['ms_per_s'][0] for w in ws]), med([w['ms_per_s'][1] for w in ws]),
        med([w['ms_per_s'][2] for w in ws]), med([w['ms_per_s'][3] for w in ws]),
        med(outside)))
print()
print('틱 한 번당 마이크로초 (방 전체):')
for cond, items in groups.items():
    ws = [w for _, w in items if w]
    print(cond, '이동 %s | 갱신 %s | 구성 %s | 전송 %s | 합 %s' % tuple(med([w['per_tick'][i] for w in ws], 0) for i in range(5)))
print()
for r, w in [x for items in groups.values() for x in items]:
    print(r['label'], r['verdict'], 'cpu', r['server_cpu_pct'], 'gap_p99', r['gap_p99_ms'], 'in_p99', r['input_p99_ms'],
          'tickms/s', None if not w else round(w['ms_per_s'][4], 1))
