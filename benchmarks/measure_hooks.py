#!/usr/bin/env python3
"""Paired MMIO/hook measurements; compile both binaries from the same mmio.cpp."""
import argparse
import hashlib
import itertools
import json
import os
from pathlib import Path
import platform
import random
import statistics
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--blocks", type=int, default=3)
    parser.add_argument("--passes", type=int, default=5)
    parser.add_argument("--cpu", type=int, default=min(os.sched_getaffinity(0)))
    parser.add_argument("--range-groups", type=int, choices=(1, 10, 100), default=10)
    args = parser.parse_args()
    if args.blocks < 1 or args.passes < 1:
        parser.error("blocks and passes must be positive")
    args.out.mkdir(parents=True, exist_ok=True)
    if any(args.out.iterdir()):
        parser.error("output directory must be empty")
    binaries = {"baseline": args.baseline.resolve(), "candidate": args.candidate.resolve()}
    metadata = dict(platform=platform.platform(), cpu=args.cpu, blocks=args.blocks,
                    range_groups=args.range_groups,
                    passes=args.passes, binaries={k: dict(path=str(v), sha256=hashlib.sha256(
                        v.read_bytes()).hexdigest()) for k, v in binaries.items()})
    (args.out / "environment.json").write_text(json.dumps(metadata, indent=2) + "\n")
    cases = list(itertools.product((False, True), ("none", "global", "view", "both"),
                                  ((0, "random"), (1, "random"), (1, "mapped")),
                                  (0, 1), (0, 1)))
    records = []
    for block in range(args.blocks):
        random.Random(3800 + block).shuffle(cases)
        for j, (logger, hook, (mixed, pattern), callbacks, write) in enumerate(cases):
            env = dict(os.environ, REGFW_BENCH_THREADS="1", REGFW_BENCH_PRE_ACCESS=hook,
                       REGFW_BENCH_RANGES=str(args.range_groups))
            env.pop("REGFW_BENCH_LOGGER", None)
            if logger:
                env["REGFW_BENCH_LOGGER"] = "1"
            checksums = []
            versions = ("baseline", "candidate") if (j + block) % 2 else ("candidate", "baseline")
            for version in versions:
                command = ["taskset", "-c", str(args.cpu), str(binaries[version]),
                           str(args.passes), "4", str(mixed), str(callbacks), pattern, str(write)]
                result = subprocess.run(command, env=env, cwd=args.out.resolve(),
                                        capture_output=True, text=True, check=True)
                values, samples, checksum, multithreaded = {}, {}, None, False
                for line in result.stdout.splitlines():
                    parts = line.split()
                    if parts[:1] == ["RESULT"]:
                        values[parts[6]] = float(parts[7])
                        samples[parts[6]] = list(map(float, parts[8:]))
                    elif parts[:1] == ["CHECKSUM"]:
                        checksum = parts[1]
                    elif parts == ["LIBC_SINGLE_THREADED", "0"]:
                        multithreaded = True
                if len(values) != 4 or checksum is None or not multithreaded:
                    raise RuntimeError(f"incomplete measurement: {result.stdout}")
                checksums.append(checksum)
                records.append(dict(block=block, logger=logger, hook=hook, mixed=mixed,
                                    pattern=pattern, callbacks=callbacks, write=write, version=version,
                                    ns=values, samples=samples, checksum=checksum))
            if checksums[0] != checksums[1]:
                raise RuntimeError("baseline/candidate checksums differ")
            (args.out / "samples.json").write_text(json.dumps(records, indent=2) + "\n")
            if (j + 1) % 24 == 0:
                print(f"block {block + 1}/{args.blocks}: {j + 1}/{len(cases)} cases", flush=True)
    summary = []
    for logger, hook, (mixed, pattern), callbacks, write in sorted(cases):
        case = dict(logger=logger, hook=hook, mixed=mixed, pattern=pattern,
                    callbacks=callbacks, write=write)
        row = dict(case)
        for version in binaries:
            matched = [r for r in records if r["version"] == version and
                       all(r[key] == value for key, value in case.items())]
            row[version + "_ns"] = statistics.median(r["ns"]["regfw"] for r in matched)
        row["delta_percent"] = (row["candidate_ns"] / row["baseline_ns"] - 1) * 100
        matched = [r for r in records if all(r[key] == value for key, value in case.items())]
        row["range_switch_ns"] = statistics.median(r["ns"]["range_switch"] for r in matched)
        row["candidate_over_range"] = row["candidate_ns"] / row["range_switch_ns"]
        summary.append(row)
    (args.out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")


if __name__ == "__main__":
    main()
