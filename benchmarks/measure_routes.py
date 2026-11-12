#!/usr/bin/env python3
"""Isolate identity/index routing and vary a hand-written range/switch reference."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import statistics
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-prefix", required=True)
    parser.add_argument("--candidate-prefix", required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--blocks", type=int, default=3)
    parser.add_argument("--passes", type=int, default=3)
    parser.add_argument("--cpu", type=int, default=min(os.sched_getaffinity(0)))
    args = parser.parse_args()
    if min(args.blocks, args.passes) < 1:
        parser.error("blocks and passes must be positive")
    args.out.mkdir(parents=True, exist_ok=True)
    if any(args.out.iterdir()):
        parser.error("output directory must be empty")
    binaries = {(version, identity, index): Path(f"{prefix}-{identity}-{index}").resolve()
                for version, prefix in (("baseline", args.baseline_prefix),
                                        ("candidate", args.candidate_prefix))
                for identity in (0, 1) for index in (0, 1)}
    metadata = dict(cpu=args.cpu, blocks=args.blocks, passes=args.passes,
                    binaries={str(k): dict(path=str(v), sha256=hashlib.sha256(v.read_bytes()).hexdigest())
                              for k, v in binaries.items()})
    (args.out / "environment.json").write_text(json.dumps(metadata, indent=2) + "\n")
    cases = [(i, x, hook, write, ranges)
             for i in (0, 1) for x in (0, 1)
             for hook in ("none", "global") for write in (0, 1)
             for ranges in ((1, 10, 100) if i and x else (10,))]
    records = []
    for block in range(args.blocks):
        random.Random(3700 + block).shuffle(cases)
        for j, (identity, index, hook, write, ranges) in enumerate(cases):
            env = dict(os.environ, REGFW_BENCH_THREADS="1", REGFW_BENCH_PRE_ACCESS=hook,
                       REGFW_BENCH_RANGES=str(ranges))
            env.pop("REGFW_BENCH_LOGGER", None)
            checksums = []
            order = ("baseline", "candidate") if (block + j) % 2 else ("candidate", "baseline")
            for version in order:
                command = ["taskset", "-c", str(args.cpu), str(binaries[version, identity, index]),
                           str(args.passes), "4", "1", "0", "mapped", str(write)]
                result = subprocess.run(command, env=env, cwd=args.out.resolve(),
                                        capture_output=True, text=True, check=True)
                ns, samples, checksum, threaded = {}, {}, None, False
                for line in result.stdout.splitlines():
                    parts = line.split()
                    if parts[:1] == ["RESULT"]:
                        ns[parts[6]] = float(parts[7])
                        samples[parts[6]] = list(map(float, parts[8:]))
                    elif parts[:1] == ["CHECKSUM"]:
                        checksum = parts[1]
                    elif parts == ["LIBC_SINGLE_THREADED", "0"]:
                        threaded = True
                if len(ns) != 4 or checksum is None or not threaded:
                    raise RuntimeError(f"incomplete result: {result.stdout}")
                checksums.append(checksum)
                records.append(dict(block=block, version=version, identity=identity, index=index,
                                    hook=hook, write=write, ranges=ranges, ns=ns,
                                    samples=samples, checksum=checksum))
            if checksums[0] != checksums[1]:
                raise RuntimeError("baseline/candidate checksums differ")
        (args.out / "samples.json").write_text(json.dumps(records, indent=2) + "\n")
        print(f"block {block + 1}/{args.blocks}: {len(cases)} cases", flush=True)
    summary = []
    for identity, index, hook, write, ranges in sorted(cases):
        case = dict(identity=identity, index=index, hook=hook, write=write, ranges=ranges)
        row = dict(case)
        matched = [r for r in records if all(r[k] == v for k, v in case.items())]
        for version in ("baseline", "candidate"):
            row[version + "_ns"] = statistics.median(r["ns"]["regfw"] for r in matched
                                                     if r["version"] == version)
        row["range_switch_ns"] = statistics.median(r["ns"]["range_switch"] for r in matched)
        row["switch_normalized_ns"] = statistics.median(r["ns"]["switch_normalized"] for r in matched)
        row["delta_percent"] = (row["candidate_ns"] / row["baseline_ns"] - 1) * 100
        summary.append(row)
    (args.out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")


if __name__ == "__main__":
    main()
