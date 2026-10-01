#!/usr/bin/env python3
"""rxeq_fixed.py: bit-exact model of the firmware's fixed-point RX equaliser (rx_eq.c).

    python3 bench/rxeq_fixed.py in.wav out.wav       run a 48 kHz mono 16-bit WAV through it
    python3 bench/rxeq_fixed.py in.wav out.wav --no-dc
                                                     the same without the DC stage
    python3 bench/rxeq_fixed.py header               quantise rx_eq_k5.h into rx_eq_k5_fixed.h
    python3 bench/rxeq_fixed.py reference            regenerate the test data in bench/data
    python3 bench/rxeq_fixed.py check                header and test data up to date (make test)
    python3 bench/rxeq_fixed.py response             fixed-point response against the float design

Needs numpy. The output is what the AIOC sends over USB with the RX equaliser on, from a
reset (recording start), sample for sample: the C code in stm32/aioc-fw/Src/rx_eq.c and this
model do the same integer arithmetic, and bench/test_rx_eq.c checks they agree bit for bit.

The arithmetic (x is the int16 input at 48 kHz, M = 8, D = 336; >> is an arithmetic shift,
that is floor; the coefficient tables are int16):

    lo[k]  = (sum_i lpq[i] x[8k - i] + 16) >> 5         low-pass and decimate (SMLALD, 64-bit sum);
                                                         lpq = round(h 2^18), lo has 13 fractional bits
    y[k]   = sum_j ((lo[k - j] fq[j]) >> 16)             F at 6 kHz (SMLAWB, each product floored);
                                                         fq = round(f 2^16), y has 13 fractional bits
    c[n]   = sum_j ((y[K - j] lpq[p + 8j]) >> 16)        interpolate (SMLAWB), n = 8K + p; with the
                                                         gain of M, c has 12 fractional bits
    v[n]   = sat30((x[n - D] << 12) + c[n])              12 fractional bits, held within 2^29
                                                         (4 times full scale)
    dc     = m >> 12; h = v - dc; m += h                 DC stage, m from 0 (about 1.9 Hz)
    out[n] = saturate16((h + 2^11) >> 12)

No intermediate can overflow, even with full-scale input everywhere (see bounds()).
"""
import argparse
import os
import re
import sys
import wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(ROOT, "stm32", "aioc-fw", "Src")
FLOAT_HEADER = os.path.join(SRC, "rx_eq_k5.h")
FIXED_HEADER = os.path.join(SRC, "rx_eq_k5_fixed.h")
DATA = os.path.join(HERE, "data")
TEST_IN = os.path.join(DATA, "rxeq_in_s16le.raw")
TEST_OUT = os.path.join(DATA, "rxeq_fixed_out_s16le.raw")
TEST_OUT_NODC = os.path.join(DATA, "rxeq_fixed_nodc_out_s16le.raw")

FS = 48000
LP_SHIFT = 18       # lpq = round(h 2^18): the largest tap, 0.1, is 26214, near full int16
F_SHIFT = 16        # fq = round(f 2^16): the largest tap, -0.35, is -22780
LO_ROUND = 5        # decimator sum (2^-18) to lo (2^-13)
MUL_SHIFT = 16      # SMLAWB: each 32 x 16 product is shifted down by 16
X_SHIFT = 12        # the direct path in v's units (2^-12)
V_LIMIT = 1 << 29   # v is held within +-2^29 (4 times full scale)
DC_SHIFT = 12       # DC stage time constant 2^12 samples (about 1.9 Hz)
OUT_SHIFT = 12      # v (2^-12) to samples


def read_float_header(path=FLOAT_HEADER):
    text = open(path).read()

    def define(name):
        return int(re.search(r"#define %s (\d+)" % name, text).group(1))

    def table(name):
        body = re.search(r"%s\[[A-Z_]+\] = \{(.*?)\};" % name, text, re.S).group(1)
        return np.array([float(v.strip().rstrip("f")) for v in body.replace("\n", " ").split(",") if v.strip()],
                        dtype=np.float32)

    m, lp_taps, f_taps, delay = define("RXEQ_M"), define("RXEQ_LP_TAPS"), define("RXEQ_F_TAPS"), define("RXEQ_DELAY")
    lp, f = table("rxeqLp"), table("rxeqF")
    assert len(lp) == lp_taps and len(f) == f_taps
    return m, lp, f, delay


def quantise(lp, f):
    """Integer tables from the float ones (round half to even, as numpy does)."""
    lpq = np.round(lp.astype(np.float64) * 2.0 ** LP_SHIFT).astype(np.int64)
    fq = np.round(f.astype(np.float64) * 2.0 ** F_SHIFT).astype(np.int64)
    assert np.abs(lpq).max() <= 32767, "low-pass taps must fit int16"
    assert np.abs(fq).max() <= 32767, "F taps must fit int16"
    return lpq, fq


def tables():
    m, lp, f, delay = read_float_header()
    assert m == 8
    lpq, fq = quantise(lp, f)
    return lpq, fq, delay


def run(x, dc=True):
    """The firmware's output for int16 samples x, from a reset."""
    v = run_v(x)
    n = len(v)
    if dc:
        h = np.empty(n, dtype=np.int64)
        m = 0
        for i, vi in enumerate(v.tolist()):
            hi = vi - (m >> DC_SHIFT)
            m += hi
            h[i] = hi
    else:
        h = v
    out = (h + (1 << (OUT_SHIFT - 1))) >> OUT_SHIFT
    return np.clip(out, -32768, 32767).astype(np.int16)


def run_v(x):
    """v, the equalised signal before the DC stage, with 12 fractional bits."""
    lpq, fq, delay = tables()
    x = np.asarray(x, dtype=np.int64)
    n = len(x)
    acc = np.convolve(x, lpq)[:n]
    lo = (acc[::8] + (1 << (LO_ROUND - 1))) >> LO_ROUND
    nk = len(lo)
    y = np.zeros(nk, dtype=np.int64)
    for j, c in enumerate(fq.tolist()):
        y[j:] += (lo[:nk - j] * c) >> MUL_SHIFT
    lpq_pad = np.concatenate([lpq, np.zeros(8 * 13 - len(lpq), dtype=np.int64)])
    corr = np.zeros(n, dtype=np.int64)
    for p in range(8):
        npk = len(range(p, n, 8))              # outputs at this phase: n = 8K + p, K = 0 ..
        cp = np.zeros(npk, dtype=np.int64)
        for j in range(13):
            c = int(lpq_pad[p + 8 * j])
            if c and npk > j:
                cp[j:] += (y[:npk - j] * c) >> MUL_SHIFT
        corr[p::8] = cp
    direct = np.concatenate([np.zeros(delay, dtype=np.int64), x])[:n]
    return np.clip((direct << X_SHIFT) + corr, -V_LIMIT, V_LIMIT - 1)


def bounds():
    """Worst-case magnitudes of the intermediates, for the comments in rx_eq.c."""
    lpq, fq, _ = tables()
    acc = 32768 * np.abs(lpq).sum()
    lo = acc / 2 ** LO_ROUND
    y = np.abs(fq).sum() * lo / 2 ** MUL_SHIFT
    branch = max(np.abs(lpq[r::8]).sum() for r in range(8))
    c = branch * y / 2 ** MUL_SHIFT
    v = 32768 * 2 ** X_SHIFT + c
    return {"decimator sum (int64)": acc, "lo (int32)": lo, "y (int32)": y,
            "interpolator sum (int32, with the direct path)": v}


def write_header():
    lpq, fq, delay = tables()
    m, lp, f, _ = read_float_header()
    with open(FIXED_HEADER, "w") as fh:
        fh.write("/*\n * Receive equaliser coefficients for the UV-K5 profile, in fixed point, used by rx_eq.c only.\n"
                 " *\n * Generated by bench/rxeq_fixed.py header from rx_eq_k5.h (which is generated by\n"
                 " * uvk5-packet-bench tools/rxeq_mr.py); do not edit by hand. The low-pass taps are\n"
                 f" * round(h 2^{LP_SHIFT}), the F taps round(f 2^{F_SHIFT}).\n */\n")
        fh.write("#ifndef RX_EQ_K5_FIXED_H_\n#define RX_EQ_K5_FIXED_H_\n\n#include <stdint.h>\n\n")
        fh.write(f"#define RXEQ_M {m}\n#define RXEQ_LP_TAPS {len(lpq)}\n#define RXEQ_F_TAPS {len(fq)}\n"
                 f"#define RXEQ_DELAY {delay}\n#define RXEQ_LP_SHIFT {LP_SHIFT}\n#define RXEQ_F_SHIFT {F_SHIFT}\n\n")
        fh.write("static const int16_t rxeqLpQ[RXEQ_LP_TAPS] = {\n"
                 + ",\n".join("    %d" % v for v in lpq) + "\n};\n\n")
        fh.write("static const int16_t rxeqFQ[RXEQ_F_TAPS] = {\n"
                 + ",\n".join("    %d" % v for v in fq) + "\n};\n\n#endif /* RX_EQ_K5_FIXED_H_ */\n")
    print(f"wrote {FIXED_HEADER}")


def read_raw(path):
    return np.fromfile(path, dtype="<i2")


def write_reference():
    x = read_raw(TEST_IN)
    run(x, dc=True).astype("<i2").tofile(TEST_OUT)
    run(x, dc=False).astype("<i2").tofile(TEST_OUT_NODC)
    print(f"wrote {TEST_OUT} and {TEST_OUT_NODC}")


def check():
    ok = True
    text = open(FIXED_HEADER).read()
    lpq, fq, delay = tables()
    got_lp = [int(v) for v in re.search(r"rxeqLpQ\[RXEQ_LP_TAPS\] = \{(.*?)\};", text, re.S).group(1).split(",")]
    got_f = [int(v) for v in re.search(r"rxeqFQ\[RXEQ_F_TAPS\] = \{(.*?)\};", text, re.S).group(1).split(",")]
    if got_lp != lpq.tolist() or got_f != fq.tolist() or f"#define RXEQ_DELAY {delay}\n" not in text:
        print("rx_eq_k5_fixed.h is out of date: run python3 bench/rxeq_fixed.py header")
        ok = False
    x = read_raw(TEST_IN)
    for path, dc in ((TEST_OUT, True), (TEST_OUT_NODC, False)):
        if not np.array_equal(read_raw(path), run(x, dc=dc)):
            print(f"{path} is out of date: run python3 bench/rxeq_fixed.py reference")
            ok = False
    if ok:
        print("rxeq_fixed: header and test data match the model")
    return ok


def response():
    """Fixed-point against float: an impulse at each of the 8 phases, averaged."""
    m, lp, f, delay = read_float_header()
    lpq, fq = quantise(lp, f)
    n = 1 << 14
    freqs = np.fft.rfftfreq(n, 1 / FS)
    resp_fixed = np.zeros(len(freqs), complex)
    for k in range(8):
        x = np.zeros(n, dtype=np.int64)
        x[200 + k] = 32767
        y = run_v(x).astype(float) / (32767 * 2 ** OUT_SHIFT)
        resp_fixed += np.fft.rfft(y) * np.exp(2j * np.pi * freqs / FS * (200 + k + delay))
    resp_fixed /= 8
    # the same structure in double precision with the unquantised float32 tables
    resp_float = np.zeros(len(freqs), complex)
    for k in range(8):
        x = np.zeros(n)
        x[200 + k] = 1.0
        lo = np.convolve(x, lp.astype(float))[:n][::8]
        yy = np.convolve(lo, f.astype(float))[:len(lo)]
        u = np.zeros(n)
        u[::8] = yy * 8
        y = np.concatenate([np.zeros(delay), x])[:n] + np.convolve(u, lp.astype(float))[:n]
        resp_float += np.fft.rfft(y) * np.exp(2j * np.pi * freqs / FS * (200 + k + delay))
    resp_float /= 8
    print(f"fixed point (no DC stage) against float, M {m}, {len(lp)}-tap low-pass, D {delay}")
    for hz in (20, 50, 100, 140, 200, 300, 500, 1000, 2000, 3000, 5000, 8000, 12000, 18000):
        i = np.argmin(np.abs(freqs - hz))
        a, b = resp_fixed[i], resp_float[i]
        print(f"  {hz:6d} Hz  float {20 * np.log10(abs(b)):+7.3f} dB {np.degrees(np.angle(b)):+7.2f} deg   "
              f"fixed {20 * np.log10(abs(a)):+7.3f} dB {np.degrees(np.angle(a)):+7.2f} deg")
    err = np.abs(resp_fixed - resp_float)
    print(f"  largest difference 20 Hz to 20 kHz: {20 * np.log10(err[(freqs > 20) & (freqs < 20000)].max()):.1f} dB "
          "(relative to 1)")
    for k, v in bounds().items():
        print(f"  worst-case |{k}|: {v:.3g}")


def read_wav(path):
    with wave.open(path, "rb") as w:
        if w.getnchannels() != 1 or w.getsampwidth() != 2:
            raise SystemExit(f"{path}: needs mono 16-bit PCM (got {w.getnchannels()} channel(s), "
                             f"{8 * w.getsampwidth()} bit)")
        if w.getframerate() != FS:
            raise SystemExit(f"{path}: needs {FS} Hz (got {w.getframerate()} Hz); the AIOC runs the "
                             "equaliser only at 48000 Hz")
        return np.frombuffer(w.readframes(w.getnframes()), dtype="<i2")


def write_wav(path, x):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(FS)
        w.writeframes(np.asarray(x, dtype="<i2").tobytes())


def main(argv):
    commands = ("header", "reference", "check", "response")
    if argv and argv[0] in commands:
        cmd = argv[0]
        if len(argv) != 1:
            raise SystemExit(f"{cmd} takes no arguments")
        if cmd == "header":
            write_header()
        elif cmd == "reference":
            write_reference()
        elif cmd == "check":
            sys.exit(0 if check() else 1)
        else:
            response()
        return
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inp", help="48 kHz mono 16-bit WAV in")
    ap.add_argument("out", help="WAV out")
    ap.add_argument("--no-dc", action="store_true", help="leave out the DC stage")
    a = ap.parse_args(argv)
    x = read_wav(a.inp)
    y = run(x, dc=not a.no_dc)
    write_wav(a.out, y)
    clipped = int(np.sum((y == 32767) | (y == -32768)))
    delay = tables()[2]
    print(f"{a.out}: {len(y)} samples, delay {delay} samples ({1000 * delay / FS:.1f} ms)"
          + (f", {clipped} samples at full scale" if clipped else ""))


if __name__ == "__main__":
    main(sys.argv[1:])
