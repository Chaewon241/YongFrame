# 게임 부하 측정 집계: raw/<csv>와 서버 로그(raw/game-server-*.log.gz)를 합쳐 조건별 중앙값(최소~최대)을 출력한다.
# 사용: python aggregate.py [game-load.csv | game-load-extra.csv]
import csv, gzip, re, statistics as st, collections, os, sys
raw = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'raw')
rows = list(csv.DictReader(open(os.path.join(raw, sys.argv[1] if len(sys.argv)>1 else 'game-load.csv'), encoding='utf-8-sig')))


def to_s(t):
    h, m, s = t.split(':')
    return int(h) * 3600 + int(m) * 60 + float(s)


stat_re = re.compile(r'^(\d\d:\d\d:\d\d\.\d\d\d) \[INFO \] \[통계\].*게임\{.*?틱=(\d+) 건너뜀=(\d+) 스냅샷=(\d+) 최대틱처리=(\d+)us[^}]*\}')


def server_ticks(label, start, end):
    lines = []
    for l in gzip.open(os.path.join(raw, 'game-server-%s.log.gz' % label), 'rt', encoding='utf-8-sig', errors='replace'):
        m = stat_re.match(l)
        if m:
            lines.append((to_s(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(5))))
    s, e = to_s(start), to_s(end)
    before = [x for x in lines if x[0] <= s]
    after = [x for x in lines if x[0] <= e]   # 측정이 끝난 뒤의 연결 정리 구간을 넣지 않도록, 끝 시각 이전의 마지막 줄까지만
    first_after = [x for x in lines if x[0] > e]
    if not before or not after:
        return None
    b, a = before[-1], after[-1]
    fa = first_after[0] if first_after else a
    return dict(ticks=a[1] - b[1], skipped=a[2] - b[2], skipped_with_teardown=fa[2] - b[2], maxtick_total=max(x[3] for x in lines))


groups = collections.OrderedDict()
for r in rows:
    r['srv'] = server_ticks(r['label'], r['measure_start'], r['measure_end'])
    groups.setdefault(int(r['rooms']), []).append(r)


def f(v, k, d=2):
    x = [float(r[k]) for r in v]
    return '%.*f (%.*f~%.*f)' % (d, st.median(x), d, min(x), d, max(x))


print('| 방 | 플레이어 | 판정 | 도착간격 p50 | p99 | 최대 | 입력반영 p50 | p99 | 최대 | 누락 | 서버CPU | 방당CPU | 메모리(MB) | 대역폭 KiB/s | 틱건너뜀 | 서버틱최대(us) | 생성기/스레드 | 스케줄지연 p99 |')
for rooms, v in groups.items():
    passes = sum(r['verdict'] == 'pass' for r in v)
    sat = sum(r['verdict'] == 'saturated' for r in v)
    skipped = [r['srv']['skipped'] for r in v if r['srv']]
    skipped_td = [r['srv']['skipped_with_teardown'] for r in v if r['srv']]
    ticks = [r['srv']['ticks'] for r in v if r['srv']]
    maxt = [r['srv']['maxtick_total'] for r in v if r['srv']]
    cpu = [float(r['server_cpu_pct']) for r in v]
    print('rooms', rooms, 'players', rooms * 4, 'pass', passes, '/3 sat', sat,
          '| gap p50', f(v, 'gap_p50_ms'), 'p99', f(v, 'gap_p99_ms'), 'max', f(v, 'gap_max_ms', 1),
          '| in p50', f(v, 'input_p50_ms'), 'p99', f(v, 'input_p99_ms'), 'max', f(v, 'input_max_ms', 1),
          '| missing', [int(r['snapshots_missing']) for r in v], 'viol', [int(r['violations']) for r in v],
          'unapplied', [int(r['inputs_unapplied']) for r in v], 'disc', [int(r['disconnects']) for r in v],
          '| srvcpu', f(v, 'server_cpu_pct', 1), 'perroom', '%.2f' % (st.median(cpu) / rooms),
          'ws_end', f(v, 'server_ws_end_mb', 1), 'growth', f(v, 'mem_growth', 3),
          'kib/s', f(v, 'recv_kib_per_s', 0),
          '| skipped(구간)', skipped, '정리포함', skipped_td, 'of', ticks, 'maxtick_us', maxt,
          '| gen/thread', f(v, 'generator_cpu_per_thread_pct', 1), 'lag p99', f(v, 'lag_p99_ms'))
print()
for r in rows:
    print(r['label'], r['verdict'], 'gap_p99', r['gap_p99_ms'], 'in_p99', r['input_p99_ms'], 'srvcpu', r['server_cpu_pct'], 'missing', r['snapshots_missing'], 'skip', r['srv'])
