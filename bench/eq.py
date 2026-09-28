#!/usr/bin/env python3
"""Design coefficients for the AIOC packet-eq transmit equaliser and print the register writes.

    eq.py                          fit 3 sections to the built-in measured K5 TX response
    eq.py --measured FILE.csv      fit to your own table instead (lines of "hz,db re 1 kHz")
    eq.py --section peak:62:-7:0.7 --section highshelf:4500:5:0.7
                                   use hand-picked RBJ sections instead of fitting
    eq.py --bypass                 print the single write that turns the equaliser off

Section types (RBJ Audio EQ Cookbook): peak:F:GAIN_DB:Q, lowshelf:F:GAIN_DB:Q,
highshelf:F:GAIN_DB:Q, hp:F:Q, lp:F:Q. The whole cascade is then scaled so that its highest
gain anywhere from 10 Hz to fs/2 is --ceiling-db (default -0.1 dB): the equaliser only ever
cuts, so a full-scale input cannot clip. The level lost in the passband is printed; make it up
with the K5's deviation setting.

Firmware format (see EQ.md): section k at 0xB0 + 5k holds b0, b1, b2, a1, a2, signed 32 bit
Q3.29; 0xBF is the control word (NSECT bits 0-1, GEN bits 8-15, FS bits 16-31). The printed
aioc_reg.py writes are RAM only: they are lost at power-off, and aioc_reg.py never stores.

Needs only numpy.
"""
import argparse
import math
import sys

import numpy as np

COEF_FRAC = 29
COEF0_ADDR = 0xB0
CTRL_ADDR = 0xBF
INFO_ADDR = 0xC8
MAX_SECTIONS = 3
SIG_HEADROOM_DB = 20 * math.log10(4)   # internal clamp is 4x full scale
AIOC_REG = "/home/tf/src/uvk5-packet-bench/tools/aioc_reg.py"

# Over-the-air TX response, AIOC rev 1.0 into the K5 mic input, packet firmware DIG path,
# 0x862 deviation, -12 dBFS, relative to 1 kHz (uvk5-packet-bench docs/results.md, 2026-09-28).
MEASURED = [
    (20, 3.0), (31.5, 5.5), (50, 6.7), (63, 6.8), (100, 5.9), (160, 4.3), (250, 2.7),
    (315, 2.1), (500, 1.2), (800, 0.6), (1000, 0.0), (1600, 0.1), (2000, 0.1), (2500, -0.5),
    (3150, -1.1), (4000, -2.4), (5000, -3.8), (6000, -5.5),
]


# ---------------------------------------------------------------- RBJ cookbook sections

def rbj(kind, fs, f0, gain_db=0.0, q=0.7071):
    """Return (b, a) normalised to a0 = 1."""
    A = 10 ** (gain_db / 40)
    w0 = 2 * math.pi * f0 / fs
    cw, sw = math.cos(w0), math.sin(w0)
    alpha = sw / (2 * q)
    sa = 2 * math.sqrt(A) * alpha
    if kind == "peak":
        b = [1 + alpha * A, -2 * cw, 1 - alpha * A]
        a = [1 + alpha / A, -2 * cw, 1 - alpha / A]
    elif kind == "lowshelf":
        b = [A * ((A + 1) - (A - 1) * cw + sa), 2 * A * ((A - 1) - (A + 1) * cw), A * ((A + 1) - (A - 1) * cw - sa)]
        a = [(A + 1) + (A - 1) * cw + sa, -2 * ((A - 1) + (A + 1) * cw), (A + 1) + (A - 1) * cw - sa]
    elif kind == "highshelf":
        b = [A * ((A + 1) + (A - 1) * cw + sa), -2 * A * ((A - 1) + (A + 1) * cw), A * ((A + 1) + (A - 1) * cw - sa)]
        a = [(A + 1) - (A - 1) * cw + sa, 2 * ((A - 1) - (A + 1) * cw), (A + 1) - (A - 1) * cw - sa]
    elif kind == "hp":
        b = [(1 + cw) / 2, -(1 + cw), (1 + cw) / 2]
        a = [1 + alpha, -2 * cw, 1 - alpha]
    elif kind == "lp":
        b = [(1 - cw) / 2, 1 - cw, (1 - cw) / 2]
        a = [1 + alpha, -2 * cw, 1 - alpha]
    else:
        raise SystemExit(f"unknown section type {kind}")
    return np.array(b) / a[0], np.array(a) / a[0]


def parse_section(text, fs):
    parts = text.split(":")
    kind = parts[0]
    try:
        nums = [float(x) for x in parts[1:]]
    except ValueError:
        raise SystemExit(f"bad section {text!r}")
    if kind in ("hp", "lp"):
        if len(nums) != 2:
            raise SystemExit(f"{kind} needs {kind}:F:Q, got {text!r}")
        return (kind, nums[0], 0.0, nums[1]), rbj(kind, fs, nums[0], 0.0, nums[1])
    if len(nums) != 3:
        raise SystemExit(f"{kind} needs {kind}:F:GAIN_DB:Q, got {text!r}")
    return (kind, nums[0], nums[1], nums[2]), rbj(kind, fs, *nums)


# ---------------------------------------------------------------- responses

def resp(sections, f, fs):
    """Complex response of a cascade [(b, a), ...] at frequencies f."""
    z = np.exp(-1j * 2 * np.pi * np.asarray(f, dtype=float) / fs)
    h = np.ones_like(z)
    for b, a in sections:
        h *= (b[0] + b[1] * z + b[2] * z * z) / (a[0] + a[1] * z + a[2] * z * z)
    return h


def db(h):
    return 20 * np.log10(np.maximum(np.abs(h), 1e-12))


def dense_grid(fs):
    return np.geomspace(10, fs / 2 * 0.9999, 4000)


def l1_gain(sections, fs, n=1 << 16):
    """Sum of |impulse response|: the worst-case peak gain for any input."""
    x = np.zeros(n)
    x[0] = 1.0
    for b, a in sections:
        y = np.zeros(n)
        x1 = x2 = y1 = y2 = 0.0
        for i in range(n):
            v = b[0] * x[i] + b[1] * x1 + b[2] * x2 - a[1] * y1 - a[2] * y2
            x2, x1, y2, y1 = x1, x[i], y1, v
            y[i] = v
        x = y
    return float(np.sum(np.abs(x)))


# ---------------------------------------------------------------- fitting

def weight(f):
    if 300 <= f <= 3200:
        return 1.0
    if 150 <= f <= 4100:
        return 0.5
    return 0.2


# Parameter vector for the fit, one row per section: (kind, f range, gain range, q range)
FIT_TEMPLATE = [
    ("peak", (30, 400), (-15, 0), (0.3, 3.0)),      # the low-frequency hump from the AIOC network
    ("peak", (3000, 6500), (0, 10), (0.3, 1.5)),    # the K5's top-end droop; a peak rather than a
                                                    # shelf, so the lift falls away above 6 kHz and
                                                    # costs about 6 dB of level instead of 11
    ("peak", (150, 6000), (-4, 4), (0.3, 1.2)),     # whatever is left, kept broad
]


def unpack(p, template):
    out = []
    for i, (kind, fr, gr, qr) in enumerate(template):
        u = 1 / (1 + np.exp(-p[3 * i: 3 * i + 3]))           # squash to 0..1
        f = math.exp(math.log(fr[0]) + u[0] * (math.log(fr[1]) - math.log(fr[0])))
        g = gr[0] + u[1] * (gr[1] - gr[0])
        q = math.exp(math.log(qr[0]) + u[2] * (math.log(qr[1]) - math.log(qr[0])))
        out.append((kind, f, g, q))
    return out


def fit_cost(params, meas_f, meas_db, w, fs):
    secs = [rbj(k, fs, f, g, q) for k, f, g, q in params]
    err = meas_db + db(resp(secs, meas_f, fs))
    c = np.sum(w * err) / np.sum(w)              # the overall level is free
    return float(np.sum(w * (err - c) ** 2))


def nelder_mead(fun, x0, step=1.0, iters=4000, tol=1e-10):
    n = len(x0)
    pts = [np.array(x0, dtype=float)]
    for i in range(n):
        p = np.array(x0, dtype=float)
        p[i] += step
        pts.append(p)
    vals = [fun(p) for p in pts]
    for _ in range(iters):
        order = np.argsort(vals)
        pts = [pts[i] for i in order]
        vals = [vals[i] for i in order]
        if abs(vals[-1] - vals[0]) < tol:
            break
        centroid = np.mean(pts[:-1], axis=0)
        xr = centroid + (centroid - pts[-1])
        fr = fun(xr)
        if fr < vals[0]:
            xe = centroid + 2 * (centroid - pts[-1])
            fe = fun(xe)
            pts[-1], vals[-1] = (xe, fe) if fe < fr else (xr, fr)
        elif fr < vals[-2]:
            pts[-1], vals[-1] = xr, fr
        else:
            xc = centroid + 0.5 * (pts[-1] - centroid)
            fc = fun(xc)
            if fc < vals[-1]:
                pts[-1], vals[-1] = xc, fc
            else:
                for i in range(1, len(pts)):
                    pts[i] = pts[0] + 0.5 * (pts[i] - pts[0])
                    vals[i] = fun(pts[i])
    i = int(np.argmin(vals))
    return pts[i], vals[i]


def fit(meas, fs, nsect):
    template = FIT_TEMPLATE[:nsect]
    meas_f = np.array([m[0] for m in meas], dtype=float)
    meas_db = np.array([m[1] for m in meas], dtype=float)
    w = np.array([weight(f) for f in meas_f])
    fun = lambda p: fit_cost(unpack(p, template), meas_f, meas_db, w, fs)
    rng = np.random.default_rng(1)
    best = None
    for start in range(12):
        x0 = np.zeros(3 * nsect) if start == 0 else rng.normal(0, 1.5, 3 * nsect)
        x, v = nelder_mead(fun, x0)
        x, v = nelder_mead(fun, x, step=0.3)
        if best is None or v < best[1]:
            best = (x, v)
    return unpack(best[0], template)


# ---------------------------------------------------------------- quantisation and output

def scale_to_ceiling(sections, fs, ceiling_db):
    """Scale the cascade so its highest gain is ceiling_db. The scale goes into the section
    with the highest own gain, so no intermediate signal is boosted more than needed."""
    grid = dense_grid(fs)
    total = np.max(db(resp(sections, grid, fs)))
    k = int(np.argmax([np.max(db(resp([s], grid, fs))) for s in sections]))
    g = 10 ** ((ceiling_db - total) / 20)
    out = list(sections)
    b, a = out[k]
    out[k] = (b * g, a)
    return out, ceiling_db - total


def quantise(sections):
    words, qsecs = [], []
    for b, a in sections:
        vals = [b[0], b[1], b[2], a[1], a[2]]
        q = []
        for v in vals:
            n = int(round(v * (1 << COEF_FRAC)))
            if not (-(1 << 31) <= n < (1 << 31)):
                raise SystemExit(f"coefficient {v} is outside Q3.29 (-4 .. +4)")
            q.append(n)
        words.append(q)
        s = 1.0 / (1 << COEF_FRAC)
        qsecs.append((np.array([q[0] * s, q[1] * s, q[2] * s]), np.array([1.0, q[3] * s, q[4] * s])))
    return words, qsecs


def ctrl_word(nsect, gen, fs_tag):
    return (nsect & 0x3) | ((gen & 0xFF) << 8) | ((fs_tag & 0xFFFF) << 16)


def load_measured(path):
    rows = []
    with open(path) as fh:
        for line in fh:
            line = line.split("#")[0].strip()
            if not line:
                continue
            f, d = line.replace(";", ",").split(",")[:2]
            rows.append((float(f), float(d)))
    return rows


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fs", type=int, default=48000, help="playback sample rate the EQ is for (default 48000)")
    ap.add_argument("--any-rate", action="store_true", help="put FS=0 in the control word: apply at any playback rate")
    ap.add_argument("--section", action="append", default=[], help="hand-picked RBJ section (repeat, up to 3)")
    ap.add_argument("--sections", type=int, default=3, choices=(1, 2, 3), help="sections to fit (default 3)")
    ap.add_argument("--measured", help="CSV of hz,db re 1 kHz to fit and predict against")
    ap.add_argument("--ceiling-db", type=float, default=-0.1, help="highest EQ gain at any frequency (default -0.1)")
    ap.add_argument("--gen", type=int, help="GEN tag 0-255 (default: derived from the coefficients); "
                                           "must differ from the value in 0xBF now, or nothing is latched")
    ap.add_argument("--bypass", action="store_true", help="print the write that turns the EQ off")
    args = ap.parse_args(argv)

    if args.bypass:
        print("# Turn the TX equaliser off (bypass, identical to stock v1.4.1):")
        print(f"python3 {AIOC_REG} write 0x{CTRL_ADDR:02X} 0x00000000")
        return

    fs = args.fs
    meas = load_measured(args.measured) if args.measured else MEASURED

    if args.section:
        if len(args.section) > MAX_SECTIONS:
            raise SystemExit("at most 3 sections")
        parsed = [parse_section(s, fs) for s in args.section]
        params = [p for p, _ in parsed]
        how = "hand-picked"
    else:
        params = fit(meas, fs, args.sections)
        how = "least-squares fit to the measured response, flat 300 Hz to 3.2 kHz weighted highest"

    raw = [rbj(k, fs, f, g, q) for k, f, g, q in params]
    scaled, gain_db = scale_to_ceiling(raw, fs, args.ceiling_db)
    words, qsecs = quantise(scaled)
    nsect = len(qsecs)

    gen = args.gen
    if gen is None:
        h = 0
        for w in words:
            for n in w:
                h = (h * 31 + (n & 0xFFFFFFFF)) & 0xFFFFFFFF
        gen = (h % 255) + 1
    ctrl = ctrl_word(nsect, gen, 0 if args.any_rate else fs)

    grid = dense_grid(fs)
    eq_max = float(np.max(db(resp(qsecs, grid, fs))))
    partial_max = [float(np.max(db(resp(qsecs[:i + 1], grid, fs)))) for i in range(nsect)]
    l1 = l1_gain(qsecs, fs)

    print(f"# TX EQ design, fs {fs} Hz, {how}")
    print("# Sections (before scaling):")
    for i, (k, f, g, q) in enumerate(params):
        extra = f"gain {g:+.2f} dB " if k not in ("hp", "lp") else ""
        print(f"#   {i}: {k:9s} {f:8.1f} Hz  {extra}Q {q:.3f}")
    print(f"# Overall scale {gain_db:+.2f} dB so the highest EQ gain anywhere is {eq_max:+.2f} dB")
    eq1k = float(db(resp(qsecs, [1000.0], fs))[0])
    print(f"# EQ gain at 1 kHz {eq1k:+.2f} dB: raise the K5 deviation by that much to keep the same level")
    print(f"# Worst-case peak gain (sum of |impulse response|) {20 * math.log10(l1):+.2f} dB; "
          f"internal headroom is {SIG_HEADROOM_DB:.0f} dB, the output saturates at full scale")
    print("# Highest gain after each section: " + ", ".join(f"{g:+.2f} dB" for g in partial_max))
    print("#")
    print("# Predicted response, dB re 1 kHz (quantised coefficients):")
    print("#      Hz   measured      EQ   corrected")
    meas_f = np.array([m[0] for m in meas], dtype=float)
    eq_db = db(resp(qsecs, meas_f, fs)) - eq1k
    corr = []
    for (f, d), e in zip(meas, eq_db):
        corr.append(d + e)
        print(f"# {f:7.1f}   {d:+6.1f}   {e:+6.2f}    {d + e:+6.2f}")
    band = [c for (f, _), c in zip(meas, corr) if 300 <= f <= 3200]
    band_m = [d for f, d in meas if 300 <= f <= 3200]
    if band:
        print(f"# Spread 300 Hz to 3.2 kHz: measured {max(band_m) - min(band_m):.2f} dB, "
              f"corrected {max(band) - min(band):.2f} dB")
    print("#")
    print("# Coefficients, Q3.29 (b0 b1 b2 a1 a2):")
    for i, w in enumerate(words):
        print("#   " + str(i) + ": " + " ".join(f"{n:+11d}" for n in w))
    print("#")
    print("# Register writes, RAM only. Coefficients first, the control word last (it commits them):")
    addr = COEF0_ADDR
    for w in words:
        for n in w:
            print(f"python3 {AIOC_REG} write 0x{addr:02X} 0x{n & 0xFFFFFFFF:08X}")
            addr += 1
    print(f"python3 {AIOC_REG} write 0x{CTRL_ADDR:02X} 0x{ctrl:08X}")
    print("# Check: 0xC8 should read 0x....%02X0%X (GEN %d, NSECT %d, bit 2 set while playing at %s)"
          % (gen, 4 | nsect, gen, nsect, f"{fs} Hz" if not args.any_rate else "any rate"))
    print(f"python3 {AIOC_REG} read 0x{INFO_ADDR:02X}")
    print("# If 0xBF already held this exact value, nothing was latched: rerun with another --gen.")
    print(f"# Revert: python3 {AIOC_REG} write 0x{CTRL_ADDR:02X} 0x00000000")


if __name__ == "__main__":
    main(sys.argv[1:])
