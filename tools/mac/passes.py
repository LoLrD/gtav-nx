import re
import sys
from collections import defaultdict

log, marks = sys.argv[1], sys.argv[2]
top = int(sys.argv[3]) if len(sys.argv) > 3 else 12
segs = []
for line in open(marks):
    m = re.match(r'(\d+):(\d+):(\d+) (\S+) line\s+(\d+)', line)
    if m:
        segs.append((m.group(4), int(m.group(5)), int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3))))
lines = open(log, errors='replace').read().split('\n')
reports = []
cur = None
for i, l in enumerate(lines):
    m = re.search(r'GPU by pass kind, ms per frame over 300 frames \(total ([\d.]+)\)', l)
    if m:
        cur = {'line': i + 1, 'total': float(m.group(1)), 'rows': []}
        reports.append(cur)
        continue
    m = re.search(r'GPUPASS +([\d.]+) ms  (.*) \(([\d.]+) passes, (\d+) draws[,)]', l)
    if m and cur:
        cur['rows'].append((m.group(2), float(m.group(1)), float(m.group(3)), int(m.group(4))))
fps = [(i + 1, float(m.group(1))) for i, l in enumerate(lines) for m in [re.search(r'\] fps=([\d.]+)', l)] if m]
drawn = [(i + 1, int(m.group(1))) for i, l in enumerate(lines) for m in [re.search(r'replay frame \d+: (\d+) drawn', l)] if m]
import os
window = int(os.environ.get('WINDOW', '100000'))


def stamp(i):
    m = re.search(r'\d+-\d+-\d+ (\d+):(\d+):(\d+)', lines[i - 1])
    return int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3)) if m else 0


for k, (name, start, t0) in enumerate(segs):
    end = segs[k + 1][1] if k + 1 < len(segs) else len(lines) + 1
    while end > start + 1 and stamp(end - 1) > t0 + window:
        end -= 1
    rs = [r for r in reports if start < r['line'] < end][1:]
    if not rs:
        print(f'== {name}: no full reports')
        continue
    n = len(rs)
    total = sum(r['total'] for r in rs) / n
    by_dl = defaultdict(float)
    by_label = defaultdict(lambda: [0.0, 0.0, 0])
    for r in rs:
        for label, ms, passes, draws in r['rows']:
            m = re.match(r'dl ([0-9A-F]{2}) ', label)
            by_dl[m.group(1) if m else label.split(' (')[0]] += ms / n
            e = by_label[label]
            e[0] += ms / n
            e[1] += passes / n
            e[2] += draws / n
    d = [v for l, v in drawn if start < l < end]
    f = [v for l, v in fps if start < l < end]
    print(f'== {name}: {n} reports, GPU total {total:.2f} ms, draws {sum(d)/max(1,len(d)):.0f}, fps {sum(f)/max(1,len(f)):.1f}')
    print('   by dl: ' + ', '.join(f'{k} {v:.2f} ({100*v/total:.0f}%)' for k, v in sorted(by_dl.items(), key=lambda x: -x[1])))
    for label, (ms, passes, draws) in sorted(by_label.items(), key=lambda x: -x[1][0])[:top]:
        print(f'   {ms:6.2f} ms {100*ms/total:4.1f}%  {label}  ({passes:.1f} passes, {draws:.0f} draws)')
