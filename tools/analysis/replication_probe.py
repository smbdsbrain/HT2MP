"""Read-only exact-Steam replication probe. No debugger, writes, or UI input.

Example: python replication_probe.py --pid-a 123 --pid-b 456
  --remote-a 0x12345678 --remote-b 0x23456789 --out build/replication-live.json
Remote addresses come from each bridge's REMOTE-ACTOR vi= diagnostic.
"""
import argparse
import ctypes as c
from ctypes import wintypes as w
import hashlib
import json
import math
from pathlib import Path
import struct
import time

STEAM_HASH = "8138aceebfd67b9ed3d8e1d209a34ded92075c5627a19dc6ccbb32fb0e4c3d36"
k = c.WinDLL("kernel32", use_last_error=True)
k.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
k.OpenProcess.restype = w.HANDLE
k.CloseHandle.argtypes = [w.HANDLE]
k.ReadProcessMemory.argtypes = [w.HANDLE, c.c_void_p, c.c_void_p, c.c_size_t, c.POINTER(c.c_size_t)]
k.ReadProcessMemory.restype = w.BOOL
k.QueryFullProcessImageNameW.argtypes = [w.HANDLE, w.DWORD, w.LPWSTR, c.POINTER(w.DWORD)]
k.QueryFullProcessImageNameW.restype = w.BOOL


class Reader:
    def __init__(self, pid):
        self.handle = k.OpenProcess(0x1010, False, pid)  # query-limited + VM_READ
        if not self.handle:
            raise c.WinError(c.get_last_error())
        path = c.create_unicode_buffer(32768)
        size = w.DWORD(len(path))
        if not k.QueryFullProcessImageNameW(self.handle, 0, path, c.byref(size)):
            raise c.WinError(c.get_last_error())
        if hashlib.sha256(Path(path.value).read_bytes()).hexdigest() != STEAM_HASH:
            raise ValueError("probe requires exact Steam executable")

    def close(self):
        k.CloseHandle(self.handle)

    def read(self, address, size):
        if not 0x10000 <= address < 0x7FFF0000 or not 0 < size <= 32768:
            raise ValueError("invalid read bounds")
        buf = c.create_string_buffer(size)
        count = c.c_size_t()
        if not k.ReadProcessMemory(self.handle, address, buf, size, c.byref(count)) or count.value != size:
            raise c.WinError(c.get_last_error())
        return buf.raw

    def ptr(self, address):
        return struct.unpack("<I", self.read(address, 4))[0]

    def vehicle(self, address):
        data = self.read(address, 0x5470)
        get = lambda fmt, off: struct.unpack_from(fmt, data, off)[0]
        deform = self.read(get("<I", 0x2540), 9 * 20)
        physics = get("<I", 0x5460)
        if self.ptr(physics + 0x29D4) != address + 0x10:
            raise ValueError("vehicle lifetime mismatch")
        chassis = self.read(physics + 0x3CA0, 27 * 20)
        wheels = get("<I", 0x28B8)
        if not 2 <= wheels <= 8:
            raise ValueError("invalid native wheel count")
        return {
            "address": hex(address), "steering": get("<b", 0x27C4),
            "lights": [get("<I", x) for x in (0x5178, 0x517C, 0x51B4, 0x51B8)],
            "deformation": [struct.unpack_from("<f", deform, i * 20 + 12)[0] for i in range(9)],
            "deformation_stage": [struct.unpack_from("<I", deform, i * 20 + 4)[0] for i in range(9)],
            "wheel_yaw_degrees": [math.degrees(math.atan2(get("<f", 0x25B8 + i * 0x30), get("<f", 0x25B4 + i * 0x30))) for i in range(2)],
            "wheel_spin_radians": [get("<f", 0x27D0 + i * 4) for i in range(wheels)],
            "wheel_visual_spin_radians": [math.atan2(get("<f", 0x25C8 + i * 0x30), get("<f", 0x25D4 + i * 0x30)) for i in range(wheels)],
            "chassis": [struct.unpack_from("<f", chassis, i * 20 + 12)[0] for i in range(27)],
            "condition": [get("<f", x + 0x24) for x in (0x52A0, 0x52D4, 0x5308, 0x533C)],
        }

    def snapshot(self, remote):
        world = self.read(self.ptr(0x7025B8), 0x240)
        return {
            "time": time.perf_counter(),
            "world": [struct.unpack_from("<d", world, x)[0] for x in (0x58, 0x68, 0x78)],
            "local": self.vehicle(self.ptr(self.ptr(0x6D2078) + 0x268)),
            "remote": self.vehicle(remote),
        }


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for side in ("a", "b"):
        p.add_argument("--pid-" + side, type=int, required=True)
        p.add_argument("--remote-" + side, type=lambda x: int(x, 0), required=True)
    p.add_argument("--seconds", type=float, default=5)
    p.add_argument("--hz", type=int, default=20)
    p.add_argument("--out", type=Path, required=True)
    args = p.parse_args()
    if not math.isfinite(args.seconds) or not 0 < args.seconds <= 120:
        p.error("seconds must be in (0, 120]")
    if not 1 <= args.hz <= 200:
        p.error("hz must be in 1..200")
    readers = []
    rows = []
    try:
        readers.append(Reader(args.pid_a))
        readers.append(Reader(args.pid_b))
        end = time.perf_counter() + args.seconds
        while time.perf_counter() < end:
            rows.append({"a": readers[0].snapshot(args.remote_a), "b": readers[1].snapshot(args.remote_b)})
            time.sleep(1.0 / args.hz)
    finally:
        for reader in readers:
            reader.close()
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(rows, indent=2), encoding="utf-8")
    summary = {"samples": len(rows)}
    if rows:
        # Default server day is 1440 real seconds; report hours directly so
        # measurements remain valid for other coordinator clock settings.
        summary["max_day_difference_hours"] = max(abs((r["a"]["world"][0] - r["b"]["world"][0] + 12) % 24 - 12) for r in rows)
        for side in ("a", "b"):
            summary[side] = {}
            for who in ("local", "remote"):
                values = [r[side][who] for r in rows]
                summary[side][who] = {
                    "steering_range": [min(x["steering"] for x in values), max(x["steering"] for x in values)],
                    "yaw_range_degrees": [min(x["wheel_yaw_degrees"][0] for x in values), max(x["wheel_yaw_degrees"][0] for x in values)],
                    "max_deformation": max(max(x["deformation"]) for x in values),
                    "last_stages": values[-1]["deformation_stage"],
                    "last_lights": values[-1]["lights"],
                }
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
