#!/usr/bin/env python3
"""Summarize HT2MP two-client sync evidence.

Inputs (all optional, any number of each):
  --log <sidecar .err.log>      bridge status lines (rate=, REMOTE-*, ONLINE ...)
  --observer <observer .csv>    ht2mp-runtime-observer CSV (local/remote vehicle poses)
  --trace <sidecar trace .csv>  sidecar network trace (remote_states / local_samples)

The report prints, per input, the numbers the acceptance criteria are judged
by: tick cadence, pose stillness at rest, arrival jitter, playback states and
receive-to-apply latency. It never modifies anything.
"""
from __future__ import annotations

import argparse
import csv
import math
import re
import statistics
import sys
from pathlib import Path


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        return float("nan")
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, int(round(fraction * (len(ordered) - 1)))))
    return ordered[index]


def report_log(path: Path) -> None:
    text = path.read_text(encoding="utf-8", errors="replace").splitlines()
    rates = [float(m.group(1)) for line in text
             if (m := re.search(r"exact-profile hook: .*rate=(\d+\.\d+)Hz", line))]
    print(f"== log {path}")
    print(f"  hook rate samples: {len(rates)}"
          + (f" first={rates[0]:.1f} last={rates[-1]:.1f} min={min(rates):.1f} max={max(rates):.1f}"
             if rates else ""))
    if rates:
        print("  rate series: " + " ".join(f"{r:.1f}" for r in rates))
    for key in ("min1s", "max1s", "maxgap", "hz", "gap"):
        found = [line for line in text if f" {key}=" in line]
        if found:
            print(f"  last {key}: {found[-1].strip()[:160]}")
    for tag in ("REMOTE-ACTOR", "REMOTE-POSE", "REMOTE-PRE", "REMOTE-POST",
                "REMOTE-TIMELINE", "REMOTE-FRAME", "REMOTE-SCENE", "ONLINE gen", "ONLINE-FOCUS"):
        found = [line for line in text if tag in line]
        if found:
            print(f"  {tag}: n={len(found)} last: {found[-1].strip()[:200]}")
    errors = [line for line in text if re.search(r"safe mode|fail-closed|abnormally|stalled", line)]
    if errors:
        print(f"  !! {len(errors)} error-ish lines, first: {errors[0].strip()[:200]}")


def _floats(rows: list[dict], name: str) -> list[float]:
    out = []
    for row in rows:
        try:
            out.append(float(row[name]))
        except (KeyError, ValueError, TypeError):
            pass
    return out


def _pose_block(rows: list[dict], prefix: str, label: str) -> None:
    xs, ys, zs = (_floats(rows, prefix + axis) for axis in ("x", "y", "z"))
    if not xs:
        print(f"  {label}: no data")
        return
    n = min(len(xs), len(ys), len(zs))
    steps = [math.dist((xs[i], ys[i], zs[i]), (xs[i - 1], ys[i - 1], zs[i - 1]))
             for i in range(1, n)]
    changes = sum(1 for s in steps if s > 0.0)
    total = sum(steps)
    print(f"  {label}: n={n} std=({statistics.pstdev(xs):.4f},{statistics.pstdev(ys):.4f},"
          f"{statistics.pstdev(zs):.4f}) span=({max(xs)-min(xs):.4f},{max(ys)-min(ys):.4f},"
          f"{max(zs)-min(zs):.4f}) path={total:.3f}m changes={changes} "
          f"maxstep={max(steps) if steps else 0.0:.4f}m")
    quat = [_floats(rows, prefix + c) for c in ("qx", "qy", "qz", "qw")]
    if all(quat) and len(quat[0]) > 1:
        m = min(len(q) for q in quat)
        angles = []
        for i in range(1, m):
            # Normalise both quaternions first: CSV rounding leaves |q| != 1,
            # and acos of a slightly short dot product reads as a fake step.
            a = [quat[c][i] for c in range(4)]
            b = [quat[c][i - 1] for c in range(4)]
            na = math.sqrt(sum(x * x for x in a)) or 1.0
            nb = math.sqrt(sum(x * x for x in b)) or 1.0
            dot = sum(x * y for x, y in zip(a, b)) / (na * nb)
            dot = max(-1.0, min(1.0, abs(dot)))
            angles.append(2.0 * math.degrees(math.acos(dot)))
        print(f"    rotation step: max={max(angles):.3f}deg mean={statistics.fmean(angles):.4f}deg "
              f"sum={sum(angles):.2f}deg")


def report_observer(path: Path) -> None:
    with path.open(newline="", encoding="utf-8", errors="replace") as handle:
        rows = list(csv.DictReader(handle))
    print(f"== observer {path} rows={len(rows)}")
    times = _floats(rows, "time_us")
    if len(times) > 1:
        dts = [(times[i] - times[i - 1]) / 1000.0 for i in range(1, len(times))]
        print(f"  sampling dt ms: mean={statistics.fmean(dts):.2f} max={max(dts):.2f}")
    statuses = {}
    for row in rows:
        statuses[row.get("status", "?")] = statuses.get(row.get("status", "?"), 0) + 1
    print(f"  transform status: {statuses}")
    _pose_block(rows, "vehicle_", "local render (+0x4ef4/+0x4f18)")
    if rows and "sim_x" in rows[0]:
        _pose_block(rows, "sim_", "local simulation (+0x204/+0x228)")
    if rows and "rv_x" in rows[0]:
        _pose_block(rows, "rv_", "remote render (+0x4ef4/+0x4f18)")
        _pose_block(rows, "rs_", "remote simulation (+0x204/+0x228)")


def report_trace(path: Path) -> None:
    with path.open(newline="", encoding="utf-8", errors="replace") as handle:
        rows = list(csv.DictReader(handle))
    print(f"== trace {path} rows={len(rows)}")
    if not rows:
        return
    recv = _floats(rows, "recv_us")
    sent = _floats(rows, "sample_time_ms")
    if len(recv) > 1:
        gaps = [(recv[i] - recv[i - 1]) / 1000.0 for i in range(1, len(recv))]
        print(f"  arrival gap ms: mean={statistics.fmean(gaps):.1f} p50={percentile(gaps, 0.5):.1f} "
              f"p95={percentile(gaps, 0.95):.1f} max={max(gaps):.1f}")
    if len(sent) > 1 and len(sent) == len(recv):
        offsets = [recv[i] / 1000.0 - sent[i] for i in range(len(recv))]
        base = min(offsets)
        jitter = [o - base for o in offsets]
        print(f"  recv-send offset ms: min={base:.1f} p50={percentile(jitter, 0.5):.1f} "
              f"p95={percentile(jitter, 0.95):.1f} max={max(jitter):.1f} (relative to min)")
        send_gaps = [sent[i] - sent[i - 1] for i in range(1, len(sent))]
        print(f"  sender gap ms: mean={statistics.fmean(send_gaps):.1f} "
              f"p95={percentile(send_gaps, 0.95):.1f} max={max(send_gaps):.1f}")
    if "source" in rows[0]:
        sources = {}
        for row in rows:
            sources[row["source"]] = sources.get(row["source"], 0) + 1
        print(f"  sources: {sources}")
    if "x" in rows[0]:
        _pose_block(rows, "", "network pose")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--log", action="append", default=[], type=Path)
    parser.add_argument("--observer", action="append", default=[], type=Path)
    parser.add_argument("--trace", action="append", default=[], type=Path)
    args = parser.parse_args(argv)
    if not (args.log or args.observer or args.trace):
        parser.print_help()
        return 2
    for path in args.log:
        report_log(path)
    for path in args.observer:
        report_observer(path)
    for path in args.trace:
        report_trace(path)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
