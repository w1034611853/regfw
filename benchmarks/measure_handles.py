#!/usr/bin/env python3
"""Alternate already-built handles benchmarks; do not build during measurement."""
import argparse
import hashlib
import json
import platform
import statistics
import subprocess
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--baseline", type=Path, required=True)
parser.add_argument("--candidate", type=Path, required=True)
parser.add_argument("--control", type=Path, help="optional intermediate optimization")
parser.add_argument("--out", type=Path, required=True)
parser.add_argument("--blocks", type=int, default=5)
parser.add_argument("--iterations", type=int, default=20_000_000)
parser.add_argument("--cpu", type=int, default=0)
args = parser.parse_args()
if args.blocks < 1 or args.iterations < 1:
    parser.error("blocks and iterations must be positive")
args.out.mkdir(parents=True, exist_ok=False)
binaries = {"baseline": args.baseline, "candidate": args.candidate}
if args.control:
    binaries["control"] = args.control
samples = []
expected = None
for block in range(args.blocks):
    order = list(binaries)
    if block % 2:
        order.reverse()
    for label in order:
        run = subprocess.run(
            ["taskset", "-c", str(args.cpu), str(binaries[label].resolve()),
             str(args.iterations)], check=True, capture_output=True, text=True)
        rows = [line.split(",") for line in run.stdout.splitlines()]
        timings = {name: float(ns) for name, ns, _ in rows}
        checksums = {name: int(checksum) for name, _, checksum in rows}
        assert len(rows) == 6, run.stdout
        for operation in ("read", "write"):
            assert len({checksums[f"{path}_{operation}"]
                        for path in ("raw", "at", "ref")}) == 1, checksums
        if expected is None:
            expected = checksums
        assert checksums == expected, (label, checksums, expected)
        samples.append(dict(block=block, binary=label, ns=timings,
                            checksums=checksums, stderr=run.stderr))
        print(f"block={block} binary={label} at_read={timings['at_read']:.4f}", flush=True)
summary = {}
for label in binaries:
    selected = [s["ns"] for s in samples if s["binary"] == label]
    summary[label] = {name: statistics.median(s[name] for s in selected)
                      for name in selected[0]}
environment = dict(platform=platform.platform(), cpu=args.cpu,
                   iterations=args.iterations, blocks=args.blocks,
                   binaries={k: dict(path=str(v.resolve()), sha256=hashlib.sha256(v.read_bytes()).hexdigest())
                             for k, v in binaries.items()})
for filename, value in (("samples", samples), ("summary", summary),
                        ("environment", environment)):
    (args.out / f"{filename}.json").write_text(json.dumps(value, indent=2) + "\n")
print(json.dumps(summary, indent=2))
