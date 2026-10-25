#!/usr/bin/env python3
"""Check an edge-magnet layout before you print sleeves.

Models each magnet as a dipole and works out what a tile's two BMM150 magnetometers read:
  * from the tile's own eight edge magnets (must not saturate the sensors),
  * the extra field from a neighbour snapped onto each side, and from the wall plate's holding
    magnets behind it (must be big and distinct).

Then it checks that every combination of joined sides gives a different reading, with margin
over sensor noise, so the firmware can tell them apart.

    python3 tools/magnet_check.py                      # default layout
    python3 tools/magnet_check.py --mag 3x1 --grade 42 --offset 20 --sensor-a -16,16 --sensor-b 16,-6 --sensor-z 5

Measure where the two BMM150 chips actually sit on your CoreBoard (mm from the board centre,
x right, y up) and pass them in. Positions here are placeholders.
"""
import argparse, itertools, math

MU0_4PI = 1e-7
BR = {35: 1.18, 42: 1.30, 52: 1.45}            # remanence, tesla
BMM150_XY, BMM150_Z = 1300.0, 2500.0           # µT full scale
NOISE = 2.0                                     # µT, after averaging a few samples

def dipole(m_vec, src, pt):
    r = [pt[i] - src[i] for i in range(3)]
    d = math.sqrt(sum(c * c for c in r)) or 1e-9
    rh = [c / d for c in r]
    mdotr = sum(m_vec[i] * rh[i] for i in range(3))
    return [MU0_4PI * (3 * mdotr * rh[i] - m_vec[i]) / d ** 3 * 1e6 for i in range(3)]   # µT

def edge_magnets(cx, cy, half, inset, offset, moment, z):
    """Eight magnets around a tile centred at (cx, cy). Each edge: N out on the clockwise side,
    S out on the other, axis along the edge normal."""
    mags = []
    for nx, ny in ((0, 1), (1, 0), (0, -1), (-1, 0)):        # top, right, bottom, left
        tx, ty = ny, -nx                                       # clockwise tangent
        for sgn in (1, -1):
            pos = (cx + nx * (half - inset) + tx * offset * sgn, cy + ny * (half - inset) + ty * offset * sgn, z)
            m = (nx * moment * sgn, ny * moment * sgn, 0.0)    # +: N pointing out
            mags.append((m, pos))
    return mags

def field_at(mags, pt):
    b = [0.0, 0.0, 0.0]
    for m, p in mags:
        f = dipole(m, p, pt)
        b = [b[i] + f[i] for i in range(3)]
    return b

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mag", default="3x1", help="disc magnet diameter x thickness, mm")
    ap.add_argument("--grade", type=int, default=42, choices=sorted(BR))
    ap.add_argument("--pitch", type=float, default=50.8, help="tile pitch (sleeve width), mm")
    ap.add_argument("--inset", type=float, default=1.2, help="magnet centre in from the sleeve edge, mm")
    ap.add_argument("--offset", type=float, default=20.0, help="magnet distance from the edge centre, mm")
    ap.add_argument("--sensor-a", default="-16,16")
    ap.add_argument("--sensor-b", default="16,-6")
    ap.add_argument("--sensor-z", type=float, default=5.0, help="sensor height relative to magnet centres, mm")
    ap.add_argument("--plate", default="0,18;0,-18", help="wall-plate holding magnets under each tile, x,y mm (; separated), axis into the tile")
    ap.add_argument("--plate-z", type=float, default=-9.0, help="plate magnet height relative to the edge magnets, mm")
    a = ap.parse_args()

    d, t = (float(v) for v in a.mag.split("x"))
    vol = math.pi * (d / 2e3) ** 2 * (t / 1e3)
    moment = BR[a.grade] * vol / (4 * math.pi * 1e-7)
    half = a.pitch / 2
    to_m = lambda xy: tuple(float(v) / 1e3 for v in xy.split(",")) + (a.sensor_z / 1e3,)
    sensors = [to_m(a.sensor_a), to_m(a.sensor_b)]
    mm = lambda v: v / 1e3

    own = edge_magnets(0, 0, mm(half), mm(a.inset), mm(a.offset), moment, 0)
    neighbours = {k: edge_magnets(dx * mm(a.pitch), dy * mm(a.pitch), mm(half), mm(a.inset), mm(a.offset), moment, 0)
                  for k, (dx, dy) in enumerate(((0, 1), (1, 0), (0, -1), (-1, 0)))}
    names = ["top", "right", "bottom", "left"]

    print(f"Magnet {a.mag} mm N{a.grade}: moment {moment*1e3:.1f} mA·m²")
    ok = True
    base = [field_at(own, s) for s in sensors]
    for si, b in enumerate(base):
        sat = abs(b[0]) > BMM150_XY or abs(b[1]) > BMM150_XY or abs(b[2]) > BMM150_Z
        ok &= not sat
        print(f"  sensor {'AB'[si]} own magnets: ({b[0]:8.1f}, {b[1]:8.1f}, {b[2]:8.1f}) µT  {'SATURATED' if sat else 'ok'}")
    sig = {k: [field_at(neighbours[k], s) for s in sensors] for k in range(4)}
    plate = [((0.0, 0.0, moment), (float(p.split(",")[0]) / 1e3, float(p.split(",")[1]) / 1e3, a.plate_z / 1e3)) for p in a.plate.split(";") if p]
    sig[4] = [field_at(plate, s) for s in sensors]
    names.append("back")
    for k in range(5):
        mags = [math.sqrt(sum(c * c for c in v)) for v in sig[k]]
        what = "the wall plate" if k == 4 else "neighbour on " + names[k]
        print(f"  {what:19s} adds {mags[0]:7.1f} µT at A, {mags[1]:7.1f} µT at B")
    # worst case: every side joined and on the plate
    full = [[base[s][i] + sum(sig[k][s][i] for k in range(5)) for i in range(3)] for s in range(2)]
    for si, b in enumerate(full):
        sat = abs(b[0]) > BMM150_XY or abs(b[1]) > BMM150_XY or abs(b[2]) > BMM150_Z
        ok &= not sat
        if sat: print(f"  sensor {'AB'[si]} with all four neighbours: SATURATED")
    # separability: smallest distance between the readings of any two different side sets
    combos = {}
    for m in range(32):
        combos[m] = [base[s][i] + sum(sig[k][s][i] for k in range(5) if m >> k & 1) for s in range(2) for i in range(3)]
    worst = min(math.dist(combos[x], combos[y]) for x, y in itertools.combinations(range(32), 2))
    margin = worst / (NOISE * math.sqrt(6))
    print(f"  closest two side sets differ by {worst:.1f} µT ({margin:.0f}× sensor noise)")
    ok &= margin > 10
    print("RESULT:", "usable" if ok else "change the layout (smaller magnets, move them away from the sensors, or a different offset)")

if __name__ == "__main__":
    main()
