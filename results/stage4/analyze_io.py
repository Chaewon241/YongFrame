# 입출력 스레드 분리 비교 집계: raw/game-load-<tag>.csv와 서버 로그(raw/game-server-<label>.log.gz)를 합쳐
# (방 수, 조건)별 합격 횟수, 지연, 누락, 서버 CPU, 틱 건너뜀, 틱 단계 시간(전송 ms/s)을 중앙값(최소~최대)으로 출력한다.
# 라벨 형식: <tag>-<조건>-r<방수>-rep<번호>.  progress 파일에 CONTAMINATED가 있는 실행은 집계에서 뺀다(따로 표시).
# 사용: python analyze_io.py [tag]   (기본 io)
import csv, gzip, os, re, statistics as st, sys, collections

tag = sys.argv[1] if len(sys.argv) > 1 else 'io'
raw = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'raw')
rows = list(csv.DictReader(open(os.path.join(raw, 'game-load-%s.csv' % tag), encoding='utf-8-sig')))

contaminated = set()
prog = os.path.join(raw, 'game-load-%s-progress.txt' % tag)
if os.path.exists(prog):
    for line in open(prog, encoding='utf-8-sig'):
        if 'CONTAMINATED' in line:
            contaminated.add(line.split()[0])


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
    return dict(secs=secs, ticks=ticks, skipped=a[2] - b[2],
                ms_per_s=[(a[i] - b[i]) / secs / 1000.0 for i in range(3, 8)])  # 이동 갱신 구성 전송 합


def med(vals, d=1):
    vals = [v for v in vals if v is not None]
    if not vals:
        return 'n/a'
    return '%.*f (%.*f~%.*f)' % (d, st.median(vals), d, min(vals), d, max(vals))


groups = collections.OrderedDict()
for r in rows:
    cond, rooms_tag, _ = r['label'].rsplit('-', 3)[1:]
    rooms = int(rooms_tag[1:])
    w = window(r['label'], r['measure_start'], r['measure_end'])
    groups.setdefault((rooms, cond), []).append((r, w, r['label'] in contaminated))

order = {'PRE': 0, 'IO0': 1, 'IO1': 2, 'IO2': 3, 'IO4': 4}
print('| 방 | 조건 | 합격 | 포화 | 도착간격 p99 ms | 입력반영 p99 ms | 누락 % | 틱 건너뜀 % | 서버 CPU % (전 스레드 합) | 틱 합 ms/s | 전송 ms/s | 생성기 CPU/스레드 % | 오염 |')
print('| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |')
for (rooms, cond) in sorted(groups, key=lambda k: (k[0], order.get(k[1], 9))):
    items = [x for x in groups[(rooms, cond)] if not x[2]]
    n_cont = sum(1 for x in groups[(rooms, cond)] if x[2])
    if not items:
        print('| %d | %s | (모두 오염) | | | | | | | | | | %d |' % (rooms, cond, n_cont))
        continue
    passes = sum(r['verdict'] == 'pass' for r, _, _ in items)
    sat = sum(r['verdict'] == 'saturated' for r, _, _ in items)
    ws = [w for _, w, _ in items if w]
    print('| %d | %s | %d/%d | %d | %s | %s | %s | %s | %s | %s | %s | %s | %d |' % (
        rooms, cond, passes, len(items), sat,
        med([float(r['gap_p99_ms']) for r, _, _ in items]),
        med([float(r['input_p99_ms']) for r, _, _ in items]),
        med([float(r['missing_rate']) * 100 for r, _, _ in items], 2),
        med([w['skipped'] / w['ticks'] * 100 for w in ws], 2),
        med([float(r['server_cpu_pct']) for r, _, _ in items]),
        med([w['ms_per_s'][4] for w in ws]),
        med([w['ms_per_s'][3] for w in ws]),
        med([float(r['generator_cpu_per_thread_pct']) for r, _, _ in items]),
        n_cont))
print()
print('실행별:')
for (rooms, cond) in sorted(groups, key=lambda k: (k[0], order.get(k[1], 9))):
    for r, w, c in groups[(rooms, cond)]:
        print(r['label'], r['verdict'], 'cpu', r['server_cpu_pct'], 'gap_p99', r['gap_p99_ms'], 'in_p99', r['input_p99_ms'],
              'missing%', round(float(r['missing_rate']) * 100, 2), 'skipped', None if not w else w['skipped'],
              'tickms/s', None if not w else round(w['ms_per_s'][4], 1), 'deliver', None if not w else round(w['ms_per_s'][3], 1),
              'CONTAMINATED' if c else '')
