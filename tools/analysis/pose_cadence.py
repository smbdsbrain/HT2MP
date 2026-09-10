#!/usr/bin/env python3
"""Read-only exact-Steam pose/cursor sampler. Never attaches a debugger.

Capture both VehicleInstance transform copies at up to 1000 Hz so short
native overwrites and pose plateaus can be distinguished from network jitter.
Requires a live vi= address from the bridge log for the specified PID.
"""
import argparse
import csv
import ctypes as c
from ctypes import wintypes as w
import json
import math
from pathlib import Path
import struct
import time


def percentile(values, fraction):
    return sorted(values)[min(len(values) - 1, int((len(values) - 1) * fraction))] if values else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--vehicle', type=lambda s: int(s, 16), required=True)
    parser.add_argument('--duration', type=float, default=60)
    parser.add_argument('--hz', type=int, default=1000)
    parser.add_argument('--csv', type=Path, required=True)
    args = parser.parse_args()
    if not 1 <= args.hz <= 1000 or not 0 < args.duration <= 3600:
        parser.error('use hz 1..1000 and duration 0..3600 seconds')

    kernel = c.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
    kernel.OpenProcess.restype = w.HANDLE
    kernel.ReadProcessMemory.argtypes = [w.HANDLE, c.c_void_p, c.c_void_p, c.c_size_t, c.POINTER(c.c_size_t)]
    kernel.ReadProcessMemory.restype = w.BOOL
    kernel.CloseHandle.argtypes = [w.HANDLE]
    handle = kernel.OpenProcess(0x10, False, args.pid)  # PROCESS_VM_READ only
    if not handle:
        raise c.WinError(c.get_last_error())

    def read(address, size):
        buffer = c.create_string_buffer(size)
        received = c.c_size_t()
        if not kernel.ReadProcessMemory(handle, address, buffer, size, c.byref(received)) or received.value != size:
            raise c.WinError(c.get_last_error())
        return buffer.raw

    winmm = c.WinDLL('winmm')
    previous = None
    last_change = [None, None, None, None]
    intervals = [[], [], [], []]
    changes = [0, 0, 0, 0]
    steps = []
    sample_count = 0
    max_copy_distance = 0.0
    max_scene_distance = 0.0
    failed = 0
    winmm.timeBeginPeriod(1)
    start = time.perf_counter()
    try:
        args.csv.parent.mkdir(parents=True, exist_ok=True)
        with args.csv.open('w', newline='', encoding='utf-8') as stream:
            writer = csv.writer(stream)
            writer.writerow(['time_s', 'sim_x', 'sim_y', 'sim_z', 'render_x', 'render_y', 'render_z',
                             'scene_x', 'scene_y', 'scene_z', 'cursor',
                             'sim_changed', 'render_changed', 'cursor_changed', 'scene_changed',
                             'copy_distance', 'scene_distance'])
            scene_node = struct.unpack('<I', read(args.vehicle + 0x154, 4))[0]
            deadline = start + args.duration
            while time.perf_counter() < deadline:
                tick = time.perf_counter()
                try:
                    sim = read(args.vehicle + 0x204, 48)
                    render = read(args.vehicle + 0x4ef4, 48)
                    cursor = read(args.vehicle + 0x2ab8, 4)
                    scene = read(scene_node + 0x40, 48)
                except OSError:
                    failed += 1
                    break  # Do not keep sampling a destroyed/reused binding.
                now = time.perf_counter() - start
                values = (sim, render, cursor, scene)
                changed = [previous is None or values[i] != previous[i] for i in range(4)]
                for i in range(4):
                    if changed[i]:
                        changes[i] += 1
                        if last_change[i] is not None:
                            intervals[i].append((now - last_change[i]) * 1000)
                        last_change[i] = now
                pos_sim = struct.unpack_from('<3f', sim, 36)
                pos_render = struct.unpack_from('<3f', render, 36)
                pos_scene = struct.unpack_from('<3f', scene, 36)
                copy_distance = math.dist(pos_sim, pos_render)
                max_copy_distance = max(max_copy_distance, copy_distance)
                scene_distance = math.dist(pos_sim, pos_scene)
                max_scene_distance = max(max_scene_distance, scene_distance)
                if previous is not None and changed[1]:
                    steps.append(math.dist(pos_render, struct.unpack_from('<3f', previous[1], 36)))
                if any(changed):
                    writer.writerow([f'{now:.6f}', *pos_sim, *pos_render, *pos_scene,
                                     struct.unpack('<I', cursor)[0],
                                     *map(int, changed), copy_distance, scene_distance])
                previous = values
                sample_count += 1
                time.sleep(max(0, 1 / args.hz - (time.perf_counter() - tick)))
    finally:
        elapsed = time.perf_counter() - start
        winmm.timeEndPeriod(1)
        kernel.CloseHandle(handle)
    result = {'pid': args.pid, 'vehicle': f'{args.vehicle:08x}', 'duration_s': elapsed,
              'samples': sample_count, 'read_failures': failed,
              'sample_hz': sample_count / elapsed, 'max_copy_distance_m': max_copy_distance,
              'max_native_scene_distance_m': max_scene_distance,
              'render_step_p95_m': percentile(steps, .95), 'render_step_max_m': max(steps, default=0)}
    for i, name in enumerate(('sim', 'render', 'cursor', 'native_scene')):
        result[name] = {'changes': changes[i], 'change_hz': changes[i] / elapsed,
                        'interval_p50_ms': percentile(intervals[i], .5),
                        'interval_p95_ms': percentile(intervals[i], .95),
                        'interval_max_ms': max(intervals[i], default=0)}
    print(json.dumps(result, indent=2))
    args.csv.with_suffix('.summary.json').write_text(json.dumps(result, indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()
