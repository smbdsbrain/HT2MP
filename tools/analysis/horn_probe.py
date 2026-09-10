"""Read-only horn voice inspection for two exact-Steam game processes.

Uses query/VM_READ only; never attaches a debugger or changes game memory.
Example: python horn_probe.py --pid-a 123 --pid-b 456 --seconds 30
  --out build/horn-voices.json
"""
import argparse
import json
import math
from pathlib import Path
import struct
import time

from replication_probe import Reader


def snapshot(reader):
    templates = [reader.ptr(address) for address in (0x6EF264, 0x6EF268, 0x6EF270)]
    clips = {reader.ptr(address + 0x1C) for address in templates if address}
    pool = struct.unpack("<32I", reader.read(0x723AA8, 128))
    voices = []
    for channel, address in enumerate(pool):
        if not address:
            continue
        fields = struct.unpack("<8I", reader.read(address, 32))
        if fields[0] == 0x64D578 and fields[7] in clips:
            voices.append({"channel": channel, "address": hex(address),
                           "remote": address not in templates, "active": fields[5],
                           "clip": fields[7], "type": fields[2], "frame": fields[3]})
    return {"time": time.perf_counter(), "voices": voices}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid-a", type=int, required=True)
    parser.add_argument("--pid-b", type=int, required=True)
    parser.add_argument("--seconds", type=float, default=30)
    parser.add_argument("--hz", type=int, default=30)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    if not math.isfinite(args.seconds) or not 0 < args.seconds <= 120:
        parser.error("seconds must be in (0, 120]")
    if not 1 <= args.hz <= 200:
        parser.error("hz must be in 1..200")
    readers, rows = [], []
    try:
        readers.append(Reader(args.pid_a))
        readers.append(Reader(args.pid_b))
        end = time.perf_counter() + args.seconds
        while time.perf_counter() < end:
            row = {}
            for side, reader in zip(("a", "b"), readers):
                try:
                    row[side] = snapshot(reader)
                except (OSError, ValueError) as error:
                    # Pool entries can change between reads; preserve evidence.
                    row[side] = {"time": time.perf_counter(), "error": str(error)}
            rows.append(row)
            time.sleep(1.0 / args.hz)
    finally:
        for reader in readers:
            reader.close()
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(rows, indent=2), encoding="utf-8")
    summary = {"samples": len(rows)}
    for side in ("a", "b"):
        samples = [row[side] for row in rows if "voices" in row[side]]
        summary[side] = {"read_errors": len(rows) - len(samples)}
        for remote in (False, True):
            counts = [sum(bool(v["active"]) and v["remote"] == remote for v in row["voices"])
                      for row in samples]
            summary[side]["remote" if remote else "local"] = {
                "max_voices": max(counts, default=0),
                "active_samples": sum(count > 0 for count in counts),
                "last_voices": counts[-1] if counts else None}
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
