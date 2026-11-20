#!/usr/bin/env python3
"""Pair already-built MMIO scalar benchmarks (builds must be complete first)."""
import argparse
import hashlib
import json
import os
import platform
import random
import statistics
import subprocess
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--baseline", type=Path, required=True)
parser.add_argument("--candidate", type=Path, required=True)
parser.add_argument("--control", type=Path)
parser.add_argument("--out", type=Path, required=True)
parser.add_argument("--blocks", type=int, default=5)
parser.add_argument("--iterations", type=int, default=3_000_000)
parser.add_argument("--cpu", type=int, default=min(os.sched_getaffinity(0)))
parser.add_argument("--quick", action="store_true", help="256 elements, finalized, no logger")
args = parser.parse_args()
if min(args.blocks, args.iterations) < 1:
    parser.error("blocks and iterations must be positive")
args.out.mkdir(parents=True, exist_ok=False)
binaries = {"baseline": args.baseline.resolve(), "candidate": args.candidate.resolve()}
if args.control:
    binaries["control"] = args.control.resolve()
cases = [(256, mode, 1, cb, w, logger)
         for mode in ("legacy", "space", "alias") for cb in ("none", "light", "info", "copy")
         for w in (0, 1) for logger in ((0,) if args.quick else (0, 1))]
if not args.quick:
    cases += [(n, mode, 1, cb, w, 0) for n in (4, 4096) for mode in ("legacy", "space")
              for cb in ("none", "info") for w in (0, 1)]
    cases += [(256, mode, 0, cb, w, 0) for mode in ("legacy", "space")
              for cb in ("none", "info") for w in (0, 1)]
environment = dict(platform=platform.platform(), cpu=args.cpu, iterations=args.iterations,
                   blocks=args.blocks, binaries={k: dict(path=str(v),
                   sha256=hashlib.sha256(v.read_bytes()).hexdigest()) for k, v in binaries.items()})
(args.out / "environment.json").write_text(json.dumps(environment, indent=2) + "\n")
records = []
expected = {}
for block in range(args.blocks):
    order = list(cases)
    random.Random(3718 + block).shuffle(order)
    for number, case in enumerate(order):
        versions = ["baseline", "candidate"]
        if args.control and case[0] == 256 and case[2] and not case[5]:
            versions.append("control")
        if (block + number) % 2:
            versions.reverse()
        for version in versions:
            cmd = ["taskset", "-c", str(args.cpu), str(binaries[version]),
                   *map(str, case[:5]), str(args.iterations), str(case[5])]
            run = subprocess.run(cmd, cwd=args.out, check=True, text=True, capture_output=True)
            lines = [line.split(",") for line in run.stdout.splitlines() if line.count(",") == 9]
            assert len(lines) == 1, run.stdout
            fields = lines[0]
            assert fields[:6] == list(map(str, case)), fields
            checksum = fields[7:]
            assert int(fields[8]) == (0 if case[3] == "none" else args.iterations), fields
            expected.setdefault(case, checksum)
            assert checksum == expected[case], (case, version, checksum, expected[case])
            records.append(dict(block=block, case=case, version=version,
                                ns=float(fields[6]), checksum=checksum))
    (args.out / "samples.json").write_text(json.dumps(records, indent=2) + "\n")
    print(f"block {block + 1}/{args.blocks}: {len(cases)} cases", flush=True)
summary = []
for case in cases:
    row = dict(zip(("elements", "mode", "finish", "callback", "write", "logger"), case))
    for version in binaries:
        values = [r["ns"] for r in records if r["version"] == version and r["case"] == case]
        if values:
            row[version] = statistics.median(values)
    row["change_percent"] = (row["candidate"] / row["baseline"] - 1) * 100
    summary.append(row)
(args.out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
