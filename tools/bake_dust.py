#!/usr/bin/env python3
"""Bake Dust's point cloud into a C header for the ESP-Mosaico firmware.

Samples the same sculpted bust (signed distance field of blended ellipsoids)
used by the web version, gives each point to one of the nine screens, then packs each point into 16 bytes.

    python3 tools/bake_dust.py            # writes main/dust_points.h
    python3 tools/bake_dust.py --head 8000 --torso 5000

The output is deterministic for a given --seed.
"""
import argparse, math, os, random

HEADY = 0.66

def ell(px, py, pz, cx, cy, cz, rx, ry, rz):
    x, y, z = (px - cx) / rx, (py - cy) / ry, (pz - cz) / rz
    k0 = math.sqrt(x * x + y * y + z * z)
    k1 = math.sqrt(x * x / (rx * rx) + y * y / (ry * ry) + z * z / (rz * rz))
    return -min(rx, ry, rz) if k1 < 1e-6 else k0 * (k0 - 1) / k1

def smin(a, b, k):
    h = max(k - abs(a - b), 0) / k
    return min(a, b) - h * h * k * 0.25

def head_sdf(x, y, z):
    ax = abs(x)
    d = ell(x, y, z, 0, 0.71, -0.05, 0.45, 0.50, 0.53)            # cranium
    d = smin(d, ell(x, y, z, 0, 0.52, 0.09, 0.35, 0.46, 0.40), 0.08)  # face mass
    d = smin(d, ell(ax, y, z, 0.13, 0.31, 0.12, 0.2, 0.18, 0.3), 0.07)  # jaw
    d = smin(d, ell(x, y, z, 0, 0.19, 0.27, 0.13, 0.1, 0.12), 0.12)  # chin
    return d

def body_sdf(x, y, z):
    ax = abs(x)
    d = ell(x, y, z, 0, 0.14, -0.01, 0.17, 0.36, 0.16)                # neck
    d = smin(d, ell(ax, y, z, 0.3, -0.16, -0.04, 0.34, 0.13, 0.17), 0.2)  # shoulder slope
    d = smin(d, ell(ax, y, z, 0.64, -0.36, 0, 0.22, 0.22, 0.2), 0.2)      # shoulder caps
    d = smin(d, ell(ax, y, z, 0.72, -0.82, 0, 0.17, 0.42, 0.18), 0.2)     # upper arms
    d = smin(d, ell(x, y, z, 0, -0.7, 0.0, 0.58, 0.6, 0.31), 0.22)        # torso
    d = smin(d, ell(x, y, z, 0, -0.44, 0.08, 0.48, 0.22, 0.24), 0.18)     # chest
    return d

def sdf(x, y, z):
    return smin(head_sdf(x, y, z), body_sdf(x, y, z), 0.09)

def project(x, y, z, iters, flat_y):
    e = 0.0015
    for _ in range(iters):
        d = sdf(x, y, z)
        gx = (sdf(x + e, y, z) - d) / e
        gy = 0.0 if flat_y else (sdf(x, y + e, z) - d) / e
        gz = (sdf(x, y, z + e) - d) / e
        g = math.sqrt(gx * gx + gy * gy + gz * gz) or 1.0
        x, y, z = x - gx / g * d, y - gy / g * d, z - gz / g * d
    return x, y, z

def grad(x, y, z):
    e, d = 0.002, sdf(x, y, z)
    gx, gy, gz = sdf(x + e, y, z) - d, sdf(x, y + e, z) - d, sdf(x, y, z + e) - d
    g = math.sqrt(gx * gx + gy * gy + gz * gz) or 1.0
    return gx / g, gy / g, gz / g

def rand_dir(rng):
    while True:
        x, y, z = (rng.uniform(-1, 1) for _ in range(3))
        d = x * x + y * y + z * z
        if 1e-4 < d <= 1:
            d = math.sqrt(d)
            return x / d, y / d, z / d

KIND_HEAD, KIND_NECK, KIND_TORSO, KIND_CORE = 0, 1, 2, 3

def bake(args):
    rng = random.Random(args.seed)
    pts = []

    def add(x, y, z, n, kind, fade, warm):
        delay = min(0.55, max(0.0, (0.9 - y) / 2.6) * 0.4 + rng.random() * 0.18)
        flags = rng.randrange(256)  # per-point random byte (chunk selection jitter)
        pts.append((x, y, z, n, kind, rng.random(), fade, warm, delay, flags))

    def region(count, box, accept, kind, band):
        made = guard = 0
        while made < count and guard < count * 20:
            guard += 1
            x = rng.uniform(box[0], box[1]); y = rng.uniform(box[2], box[3]); z = rng.uniform(box[4], box[5])
            x, y, z = project(x, y, z, 4, False)
            if rng.random() < band:
                y = round(y * 44) / 44
                x, y, z = project(x, y, z, 3, True)
            if abs(sdf(x, y, z)) > 0.006 or not accept(y):
                continue
            a, b, c = grad(x, y, z)
            fade, warm = 1.0, 0.0
            if kind == KIND_HEAD:
                warm = max(0, (c - 0.35) / 0.65) * max(0, 1 - abs(y - 0.5) / 0.34) * (1 - min(1, abs(x) / 0.4))
            elif kind == KIND_TORSO:
                fade = min(1, max(0, (y + 1.3) / 0.6)) ** 1.4
            elif kind == KIND_NECK:
                fade = 0.9
            add(x, y, z, (a, b, c), kind, fade, warm)
            made += 1

    region(args.head, [-0.52, 0.52, 0.08, 1.26, -0.62, 0.6], lambda y: y > 0.13, KIND_HEAD, 0.4)
    region(args.neck, [-0.65, 0.65, -0.28, 0.2, -0.3, 0.3], lambda y: -0.22 < y <= 0.13, KIND_NECK, 0.3)
    region(args.torso, [-1.12, 1.12, -1.32, -0.1, -0.4, 0.45], lambda y: -1.3 < y <= -0.22, KIND_TORSO, 0.15)
    for _ in range(args.core):
        a, b, c = rand_dir(rng)
        r = rng.random() ** (1 / 3)
        add(a * r * 0.27, b * r * 0.36 + HEADY - 0.04, c * r * 0.3 + 0.06, (0, 0, 1), KIND_CORE, 0.9, 1.0)
    assign_groups(pts, rng)
    return pts

def assign_groups(pts, rng):
    """Give each dot to one screen's ninth, spread evenly over the whole body, so a lifted
    screen drains Dust evenly instead of biting a hole. Mirrors the web simulator."""
    for j, p in enumerate(pts):
        pts[j] = p + (rng.randrange(9),)

def q16(v): return max(-32768, min(32767, int(round(v * 8192))))
def q8s(v): return max(-127, min(127, int(round(v * 127))))
def q8u(v): return max(0, min(255, int(round(v * 255))))

def write_header(pts, path, args):
    lines = [
        "// Generated by tools/bake_dust.py — do not edit by hand.",
        f"// seed={args.seed} head={args.head} neck={args.neck} torso={args.torso} core={args.core}",
        "#pragma once",
        '#include "dust_render.h"',
        f"#define DUST_POINT_COUNT {len(pts)}",
        "static const dust_point_t DUST_POINTS[DUST_POINT_COUNT] = {",
    ]
    for (x, y, z, n, kind, ph, fade, warm, delay, flags, group) in pts:
        lines.append("{%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d}," % (
            q16(x), q16(y), q16(z), q8s(n[0]), q8s(n[1]), q8s(n[2]),
            kind, q8u(ph), q8u(fade), q8u(warm), q8u(delay / 0.55), flags, group))
    lines.append("};")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--head", type=int, default=6500)
    ap.add_argument("--neck", type=int, default=700)
    ap.add_argument("--torso", type=int, default=4200)
    ap.add_argument("--core", type=int, default=1100)
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "..", "main", "dust_points.h"))
    a = ap.parse_args()
    p = bake(a)
    write_header(p, a.out, a)
    print(f"wrote {len(p)} points to {os.path.normpath(a.out)} ({len(p) * 16 // 1024} KB)")
