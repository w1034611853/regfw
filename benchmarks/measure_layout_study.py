#!/usr/bin/env python3
"""Pair immutable build snapshots after all compilation/test activity stops."""
import argparse, os, json, hashlib, subprocess, statistics, random
from pathlib import Path
p = argparse.ArgumentParser()
p.add_argument('--base', required=True)
p.add_argument('--candidate', required=True)
p.add_argument('--out', required=True)
p.add_argument('--blocks', type=int, default=4)
p.add_argument('--iterations', type=int, default=3000000)
p.add_argument('--cpu', type=int, default=min(os.sched_getaffinity(0)))
p.add_argument('--groups', nargs='+', choices=['scalar', 'mapped', 'dispatch', 'memory', 'init', 'handles', 'errors'], default=['scalar', 'mapped', 'dispatch', 'memory', 'init', 'handles'])
p.add_argument('--cases', nargs='+', help='Restrict to exact case names for follow-up measurements')
a = p.parse_args()
if min(a.blocks, a.iterations) < 1:
    p.error('blocks and iterations must be positive')
if a.cpu not in os.sched_getaffinity(0):
    p.error('selected CPU is outside process affinity')
root = Path.cwd()
out = (root / a.out).resolve()
out.mkdir(parents=True, exist_ok=False)
versions = {'base': (root / a.base).resolve(), 'candidate': (root / a.candidate).resolve()}
cases = []

def add(name, binary, args, kind, env=None):
    cases.append(dict(name=name, binary=binary, args=list(map(str, args)), kind=kind, env=env or {}))
if 'scalar' in a.groups:
    for mode in ['legacy', 'space', 'alias']:
        for cb in ['none', 'light', 'info', 'copy']:
            for w in [0, 1]:
                add(f'scalar-{mode}-{cb}-{w}', 'scalar', [256, mode, 1, cb, w, a.iterations, 0], 'scalar')
    for w in [0, 1]:
        add(f'scalar-large-info-{w}', 'scalar', [4096, 'space', 1, 'info', w, a.iterations, 0], 'scalar')
if 'mapped' in a.groups:
    for cb in [0, 1]:
        for w in [0, 1]:
            add(f'mapped-cb{cb}-{w}', 'mmio', [3, 4, 1, cb, 'mapped', w], 'mapped', {'REGFW_BENCH_THREADS': '1', 'REGFW_BENCH_RANGES': '10', 'REGFW_BENCH_PRE_ACCESS': 'none'})
    add('mapped-hooks', 'mmio', [3, 4, 1, 1, 'mapped', 1], 'mapped', {'REGFW_BENCH_THREADS': '1', 'REGFW_BENCH_RANGES': '10', 'REGFW_BENCH_PRE_ACCESS': 'both'})
if 'dispatch' in a.groups:
    for mode, views, reserve, local, cb in [('dense', 1, 0, 0, 0), ('dense', 64, 0, 0, 0), ('dense', 64, 128, 0, 0), ('dense', 64, 128, 1, 0), ('sparse', 64, 0, 0, 0), ('sparse', 64, 128, 1, 1), ('resonant', 64, 0, 0, 0), ('high', 64, 0, 0, 0), ('high', 64, 0, 0, 1)]:
        for w in [0, 1]:
            add(f'dispatch-{mode}-v{views}-r{reserve}-l{local}-c{cb}-w{w}', 'dispatcher', [mode, views, reserve, local, w, cb, a.iterations, 0], 'dispatch')
if 'memory' in a.groups:
    for mode in ['large', 'wide', 'bounded']:
        add(f'memory-{mode}', 'dispatcher', [mode, 9, 0, 0, 0, 0, 10000, 1], 'memory', {'REGFW_ENTITY_HASH_STATS': '1'})
if 'init' in a.groups:
    for name in ['storage4096', 'dag-full']:
        add(f'init-{name}', 'init', [name, 3], 'init')
    add('init-poe', 'poe', [3], 'poe')
if 'handles' in a.groups:
    add('handles', 'handles', [a.iterations], 'handles')
if 'errors' in a.groups:
    for probe in ('hole', 'outside', 'unaligned', 'width'):
        for w in (0, 1):
            add(f'error-{probe}-{w}', 'errors', [probe, w, a.iterations], 'error')
if a.cases:
    selected = set(a.cases)
    available = {case['name'] for case in cases}
    if selected - available:
        p.error('unknown cases: ' + ', '.join(sorted(selected - available)))
    cases = [case for case in cases if case['name'] in selected]
metadata = dict(args=vars(a), versions={k: dict(path=str(v), revision=json.loads((v / 'revision.json').read_text()), binaries={n: hashlib.sha256((v / n).read_bytes()).hexdigest() for n in sorted({c['binary'] for c in cases})}) for k, v in versions.items()})
(out / 'environment.json').write_text(json.dumps(metadata, indent=2) + '\n')

def parse(case, stdout, stderr):
    lines = [line.split() for line in stdout.splitlines()]
    kind = case['kind']
    metrics = {}
    checks = []
    if kind == 'scalar':
        f = stdout.strip().split(',')
        assert len(f) == 10
        metrics['ns'] = float(f[6])
        checks = f[:6] + f[7:]
    elif kind == 'error':
        f = stdout.strip().split()
        assert len(f) == 3 and f[0] == 'ERROR', stdout
        metrics['ns'] = float(f[1])
        checks = [f[2]]
    elif kind == 'mapped':
        for f in lines:
            if f and f[0] == 'RESULT':
                metrics[f[6] + '_ns'] = float(f[7])
            elif f and f[0] == 'CHECKSUM':
                checks = f[1:]
        assert len(metrics) == 4 and checks
    elif kind in ['dispatch', 'memory']:
        for f in lines:
            if f and f[0] == 'ACCESS':
                metrics['ns'] = float(f[1])
                checks += f[2:]
            elif f and f[0] == 'INIT':
                keys = ['init_ns', 'finish_ns', 'heap_bytes', 'rss_kb', 'summary_bytes', 'full_bytes', 'summary_ns', 'full_ns', 'entities', 'data_count']
                metrics.update(zip(keys, map(int, f[1:])))
                checks += f[5:7] + f[9:11]
        for line in stderr.splitlines():
            if line.startswith('ENTITY_HASH_STATS'):
                for k, v in (part.split('=') for part in line.split()[1:]):
                    metrics[k] = int(v)
        assert 'init_ns' in metrics
    elif kind in ['init', 'poe']:
        rows = []
        for f in lines:
            if kind == 'init' and f and (f[0] == 'RESULT'):
                rows.append(dict(init_ns=int(f[3]), finish_ns=int(f[5]), summary_ns=int(f[6]), rss_kb=int(f[12])))
                checks += f[9:12]
            elif kind == 'poe' and f and (f[0] == 'POE'):
                rows.append(dict(init_ns=int(f[2]), finish_ns=int(f[3]), summary_ns=int(f[4]), full_ns=int(f[5]), heap_bytes=int(f[6]), rss_kb=int(f[7])))
                checks += f[8:12]
        assert len(rows) == 3
        for k in rows[0]:
            metrics[k] = statistics.median((row[k] for row in rows))
        metrics['cold_init_ns'] = rows[0]['init_ns']
    elif kind == 'handles':
        for line in stdout.splitlines():
            f = line.split(',')
            assert len(f) == 3
            metrics[f[0] + '_ns'] = float(f[1])
            checks.append(f[2])
        assert len(metrics) == 6
    return (metrics, checks)
samples = []
expected = {}
for block in range(a.blocks):
    order = cases[:]
    random.Random(20261120 + block).shuffle(order)
    for number, case in enumerate(order):
        names = ['base', 'candidate'] if (block + number) % 2 == 0 else ['candidate', 'base']
        for version in names:
            env = dict(os.environ, **case['env'])
            if 'REGFW_ENTITY_HASH_STATS' not in case['env']:
                env.pop('REGFW_ENTITY_HASH_STATS', None)
            env.pop('REGFW_BENCH_LOGGER', None)
            cmd = ['taskset', '-c', str(a.cpu), str(versions[version] / case['binary']), *case['args']]
            r = subprocess.run(cmd, cwd=out, env=env, text=True, capture_output=True)
            if r.returncode:
                raise RuntimeError((case, version, r.returncode, r.stdout, r.stderr))
            metrics, checks = parse(case, r.stdout, r.stderr)
            expected.setdefault(case['name'], checks)
            assert checks == expected[case['name']], (case, version, checks, expected[case['name']])
            samples.append(dict(block=block, case=case['name'], kind=case['kind'], version=version, metrics=metrics, checks=checks, stdout=r.stdout, stderr=r.stderr))
        (out / 'samples.json').write_text(json.dumps(samples, indent=2) + '\n')
    print('block', block + 1, '/', a.blocks, flush=True)
summary = []
for case in cases:
    row = dict(case=case['name'], kind=case['kind'])
    groups = {v: [s for s in samples if s['case'] == case['name'] and s['version'] == v] for v in versions}
    for metric in groups['base'][0]['metrics']:
        b = statistics.median((s['metrics'][metric] for s in groups['base']))
        c = statistics.median((s['metrics'][metric] for s in groups['candidate']))
        row[metric] = dict(base=b, candidate=c, change_percent=(c / b - 1) * 100 if b else None)
    summary.append(row)
(out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
