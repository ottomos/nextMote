#!/usr/bin/env python3
"""Emulate a Wii game's WPAD traffic through the DolphinBar and check every reply.

Usage: game_emu.py /dev/hidrawN seconds [mode] [continuous]
"""
import os, select, sys, time

node = sys.argv[1]
duration = float(sys.argv[2])
mode = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x33
cont = (sys.argv[4] != "0") if len(sys.argv) > 4 else True
fd = os.open(node, os.O_RDWR)

stats = {}
data_times = []
gaps = []
last_data = None
pending = []  # (expected_id, sent_time, label)
fails = []
latency = {}


def drain(timeout):
    global last_data
    end = time.monotonic() + timeout
    while True:
        left = end - time.monotonic()
        if left <= 0:
            return
        r, _, _ = select.select([fd], [], [], left)
        if not r:
            return
        try:
            pkt = os.read(fd, 64)
        except OSError as e:
            fails.append((time.monotonic() - t0, f"read error {e}"))
            time.sleep(0.05)
            continue
        now = time.monotonic()
        rid = pkt[0]
        stats[rid] = stats.get(rid, 0) + 1
        if rid >= 0x30:
            if last_data is not None:
                gaps.append(now - last_data)
            last_data = now
        for i, (exp, st, label) in enumerate(pending):
            if exp == rid:
                latency.setdefault(label, []).append(now - st)
                pending.pop(i)
                break


def send(pkt, expect, label):
    try:
        os.write(fd, bytes(pkt))
    except OSError as e:
        fails.append((time.monotonic() - t0, f"write {label} error {e}"))
        return
    if expect is not None:
        pending.append((expect, time.monotonic(), label))


def expire():
    now = time.monotonic()
    for item in list(pending):
        if now - item[1] > 1.0:
            fails.append((now - t0, f"no reply 0x{item[0]:02x} for {item[2]}"))
            pending.remove(item)


t0 = time.monotonic()
# Startup like WPAD: status, calibration read, extension id read, IR init, mode.
send([0x15, 0x00], 0x20, "status")
drain(0.2)
send([0x17, 0x00, 0x00, 0x00, 0x16, 0x00, 0x0a], 0x21, "read calib")
drain(0.2)
send([0x17, 0x04, 0xa4, 0x00, 0xfa, 0x00, 0x06], 0x21, "read ext id")
drain(0.2)
send([0x13, 0x06], 0x22, "ir enable")
drain(0.05)
send([0x1a, 0x06], 0x22, "ir enable2")
drain(0.05)
for addr, val in ((0x30, [0x01]), (0x00, [0x02, 0, 0, 0x71, 0x01, 0, 0xaa, 0, 0x64]),
                  (0x1a, [0x63, 0x03]), (0x33, [0x03]), (0x30, [0x08])):
    pl = [0x16, 0x04, 0xb0, 0x00, addr, len(val)] + val + [0] * (16 - len(val))
    send(pl, 0x22, f"write b000{addr:02x}")
    drain(0.05)
send([0x11, 0x10], None, "leds")
drain(0.05)
send([0x12, (0x04 if cont else 0x00) | 0x02, mode], 0x22, f"mode 0x{mode:02x}")
drain(0.2)

# Steady state: periodic status/read/LED/rumble like a game.
k = 0
while time.monotonic() - t0 < duration:
    k += 1
    if k % 10 == 0:
        send([0x15, 0x00], 0x20, "status")
    elif k % 10 == 3:
        send([0x17, 0x00, 0x00, 0x00, 0x16, 0x00, 0x0a], 0x21, "read calib")
    elif k % 10 == 6:
        send([0x10, 0x02], 0x22, "rumble ack")
    elif k % 10 == 8:
        send([0x11, 0x10], None, "leds")
    drain(0.1)
    expire()
drain(1.1)
expire()
os.close(fd)

el = time.monotonic() - t0
print(f"elapsed {el:.1f}s mode 0x{mode:02x} cont={cont}")
print("reports:", {f"0x{k:02x}": v for k, v in sorted(stats.items())})
n = sum(v for k, v in stats.items() if k >= 0x30)
print(f"data rate {n/el:.1f}/s")
if gaps:
    gs = sorted(gaps)
    print(f"data gaps ms: median {gs[len(gs)//2]*1e3:.1f} p99 {gs[int(len(gs)*0.99)]*1e3:.1f} max {gs[-1]*1e3:.1f}"
          f"  >100ms:{sum(g>0.1 for g in gs)} >500ms:{sum(g>0.5 for g in gs)}")
for label, ls in sorted(latency.items()):
    ls.sort()
    print(f"  {label:12s} n={len(ls):4d} med {ls[len(ls)//2]*1e3:6.1f}ms max {ls[-1]*1e3:6.1f}ms")
print(f"FAILURES {len(fails)}")
for f in fails[:30]:
    print(f"  t={f[0]:.2f}s {f[1]}")
