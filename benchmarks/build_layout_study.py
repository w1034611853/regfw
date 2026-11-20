#!/usr/bin/env python3
"""Build identical benchmark consumers against one matching SDK/library revision."""
import argparse
import hashlib
import json
import shutil
import subprocess
import time
from pathlib import Path
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--sdk', type=Path, required=True)
parser.add_argument('--library', type=Path, required=True)
parser.add_argument('--out', type=Path, required=True)
parser.add_argument('--compiler', default='g++')
parser.add_argument('--hlog', type=Path)
parser.add_argument('--names', nargs='+', choices=['scalar', 'mmio', 'handles', 'dispatcher', 'init', 'poe'], default=['scalar', 'mmio', 'handles', 'dispatcher', 'init', 'poe'])
args = parser.parse_args()
sdk, archive, out = (args.sdk.resolve(), args.library.resolve(), args.out.resolve())
source = Path(__file__).resolve().parent
hlog = (args.hlog or sdk / 'third_party/hlog').resolve()
out.mkdir(parents=True, exist_ok=False)
shutil.copytree(sdk / 'include', out / 'include')
shutil.copy2(archive, out / 'libregfw.a')
revision = subprocess.check_output(['git', '-C', str(sdk), 'rev-parse', 'HEAD'], text=True).strip()
(out / 'revision.json').write_text(json.dumps(dict(commit=revision, library_sha256=hashlib.sha256(archive.read_bytes()).hexdigest()), indent=2) + '\n')
sources = dict(scalar='mmio_scalar.cpp', mmio='mmio.cpp', handles='handles.cpp', dispatcher='dispatch_layout.cpp', init='init.cpp', poe='poe_init.cpp')
for name in args.names:
    obj = out / (name + '.o')
    flags = [args.compiler, '-std=c++20', '-O3', '-DNDEBUG', '-fno-strict-aliasing', '-I' + str(out / 'include'), '-I' + str(hlog / 'include')]
    if name == 'mmio':
        flags += ['-DREGFW_STATIC_BENCH=1', '-DREGFW_BENCH_IDENTITY=1', '-DREGFW_BENCH_INDEX=1']
    commands = [flags + ['-c', str(source / sources[name]), '-o', str(obj)], [args.compiler, '-O3', '-fno-strict-aliasing', str(obj), str(out / 'libregfw.a'), '-L' + str(hlog / 'lib'), '-Wl,-rpath,' + str(hlog / 'lib'), '-lhlog', '-pthread', '-o', str(out / name)]]
    records = []
    for cmd in commands:
        start = time.monotonic()
        run = subprocess.run(cmd, text=True, capture_output=True)
        records.append(dict(command=cmd, seconds=time.monotonic() - start, code=run.returncode, stdout=run.stdout, stderr=run.stderr))
        (out / (name + '-build.json')).write_text(json.dumps(records, indent=2) + '\n')
        if run.returncode:
            print(run.stderr)
            raise SystemExit(run.returncode)
    print('built', name, flush=True)
