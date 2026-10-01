/*
 * Host unit tests for the RX equaliser (stm32/aioc-fw/Src/rx_eq.c).
 * Build and run: make -C bench test   (or see the Makefile next to this file)
 *
 * The firmware file is compiled unchanged, with the host gcc and the undefined-behaviour and
 * address sanitizers. The main test feeds a real UV-K5 recording (data/rxeq_in_s16le.raw,
 * 120000 samples of 9600 baud packets at 48 kHz) through it from a reset and compares the
 * result, bit for bit, with bench/rxeq_fixed.py's model of the fixed-point arithmetic for the
 * same input (data/rxeq_fixed_out_s16le.raw; rxeq_fixed.py check, which make test runs,
 * confirms the file is the model's output).
 *
 * This file is built twice (bench/Makefile):
 *   - test_rx_eq, rx_eq.c as the firmware has it;
 *   - test_rx_eq_core, with RXEQ_NO_DC_STAGE: without the DC stage, against the model without
 *     it (data/rxeq_fixed_nodc_out_s16le.raw), and also loosely against the float design,
 *     uvk5-packet-bench tools/rxeq_mr.py with the 97-tap low-pass (data/rxeq_ref_out_s16le.raw,
 *     rounded to int16): within 1 or 2 LSB.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rx_eq.h"

#define FS 48000
#define PI 3.14159265358979323846

/* The correction path's delay, from rx_eq_k5_fixed.h (not included here: it defines the tables) */
#define DELAY 336

static int failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static int16_t Step(rxeq_t *eq, uint32_t ctrl, uint32_t fs, int16_t x)
{
    RxEq_Poll(eq, ctrl, fs);
    return RxEq_Process(eq, x);
}

/* Stream start with the control register as it is, like usb_audio.c does */
static void Start(rxeq_t *eq, uint32_t ctrl, uint32_t fs)
{
    RxEq_Init(eq);
    RxEq_Reset(eq, ctrl, fs);
}

static int16_t Sine(double f, double ampl, long n)
{
    return (int16_t) lrint(ampl * 32767 * sin(2 * PI * f * n / FS));
}

static uint32_t rngState = 12345;
static uint32_t Rand(void)
{
    rngState ^= rngState << 13; rngState ^= rngState >> 17; rngState ^= rngState << 5;
    return rngState;
}

static long ReadRaw(const char *path, int16_t *buf, long max)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("  cannot open %s (run the tests from bench/)\n", path);
        return -1;
    }
    long n = 0;
    uint8_t b[2];
    while (n < max && fread(b, 1, 2, f) == 2) {
        buf[n++] = (int16_t) (uint16_t) (b[0] | (b[1] << 8));
    }
    fclose(f);
    return n;
}

/* ------------------------------------------------------------------------------------------ */

#define REF_LEN 120000
static int16_t refIn[REF_LEN], refOut[REF_LEN];

static long Compare(const int16_t *want, const char *what, int maxAllowed, double rmsAllowed)
{
    rxeq_t eq;
    Start(&eq, RXEQ_PROFILE_K5, FS);
    int maxDiff = 0;
    long nDiff = 0;
    double sumSq = 0;
    for (long n = 0; n < REF_LEN; n++) {
        int16_t y = Step(&eq, RXEQ_PROFILE_K5, FS, refIn[n]);
        int d = abs(y - want[n]);
        if (d > maxDiff) maxDiff = d;
        if (d) nDiff++;
        sumSq += (double) d * d;
    }
    double rms = sqrt(sumSq / REF_LEN);
    printf("  against %s: max difference %d LSB, rms %.4f LSB, %ld of %d samples differ, %u saturated\n",
           what, maxDiff, rms, nDiff, REF_LEN, eq.clips);
    CHECK(maxDiff <= maxAllowed, "%s: max difference %d LSB", what, maxDiff);
    CHECK(rms <= rmsAllowed, "%s: rms difference %.4f LSB", what, rms);
    uint32_t st = RxEq_Status(&eq);
    CHECK(st == (RXEQ_PROFILE_K5 | RXEQ_STATUS_ACTIVE_MASK), "status 0x%08X", st);
    return nDiff;
}

static void TestReference(void)
{
    printf("matches the fixed-point model on a UV-K5 recording\n");
    long n1 = ReadRaw("data/rxeq_in_s16le.raw", refIn, REF_LEN);
#ifndef RXEQ_NO_DC_STAGE
    long n2 = ReadRaw("data/rxeq_fixed_out_s16le.raw", refOut, REF_LEN);
#else
    long n2 = ReadRaw("data/rxeq_fixed_nodc_out_s16le.raw", refOut, REF_LEN);
#endif
    CHECK(n1 == REF_LEN && n2 == REF_LEN, "test data missing or short (%ld, %ld samples)", n1, n2);
    if (n1 != REF_LEN || n2 != REF_LEN) return;

    /* The equaliser does change the signal: guard against comparing two passthroughs */
    long changed = 0;
    for (long n = 0; n < REF_LEN; n++) if (refOut[n] != refIn[n]) changed++;
    CHECK(changed > REF_LEN / 2, "the model's output is mostly the input (%ld samples changed)", changed);

#ifndef RXEQ_NO_DC_STAGE
    Compare(refOut, "bench/rxeq_fixed.py", 0, 0.0);
#else
    Compare(refOut, "bench/rxeq_fixed.py --no-dc", 0, 0.0);
    long n3 = ReadRaw("data/rxeq_ref_out_s16le.raw", refOut, REF_LEN);
    CHECK(n3 == REF_LEN, "float reference missing or short");
    if (n3 == REF_LEN) Compare(refOut, "the float design (rxeq_mr.py, 97 taps)", 2, 0.5);
#endif
}

static void TestBypass(void)
{
    printf("bypass is bit-exact\n");
    static const struct { uint32_t ctrl, fs; const char *what; } cases[] = {
        { RXEQ_PROFILE_NONE, FS, "off" },
        { RXEQ_PROFILE_K5, 44100, "K5 at 44100 Hz" },
        { RXEQ_PROFILE_K5, 16000, "K5 at 16000 Hz" },
        { 2, FS, "reserved profile 2" },
        { 0x101, FS, "reserved 0x101" },
        { 0xFFFFFFFFUL, FS, "reserved 0xFFFFFFFF" },
    };
    rxeq_t eq;
    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        long bad = 0;
        Start(&eq, cases[c].ctrl, cases[c].fs);
        for (long n = 0; n < 50000; n++) {
            int16_t x = (int16_t) Rand();
            if (n % 1000 == 0) x = (n % 2000) ? 32767 : -32768;
            if (Step(&eq, cases[c].ctrl, cases[c].fs, x) != x) bad++;
        }
        CHECK(bad == 0, "%s: %ld samples changed", cases[c].what, bad);
        CHECK(RxEq_Status(&eq) == 0, "%s: status 0x%08X", cases[c].what, RxEq_Status(&eq));
    }

    /* Power-up state (Init only, no Reset) is bypass too */
    long bad = 0;
    RxEq_Init(&eq);
    for (long n = 0; n < 10000; n++) {
        int16_t x = (int16_t) Rand();
        if (RxEq_Process(&eq, x) != x) bad++;
    }
    CHECK(bad == 0, "after init: %ld samples changed", bad);
}

typedef struct { int maxStep, peak; } glitch_t;

static glitch_t Measure(const int16_t *y, long from, long to)
{
    glitch_t g = { 0, 0 };
    for (long n = from; n < to; n++) {
        int d = abs(y[n] - y[n - 1]);
        if (d > g.maxStep) g.maxStep = d;
        if (abs(y[n]) > g.peak) g.peak = abs(y[n]);
    }
    return g;
}

#define LEN (2 * FS)
static int16_t outOn[LEN], outSw[LEN];

#ifndef RXEQ_NO_DC_STAGE
/* Switching on, the DC stage starts from rest (rx_eq.c). Its 80 ms time constant then leaves
 * a slow offset that dies away over a few hundred ms: at most a few times fc / f of the
 * signal (fc = 2 Hz), so about 1% at 300 Hz. No step, just a little extra peak */
#define DC_SETTLE       (FS / 2)
#define DC_PEAK(peak, f) ((int) ((peak) * 4.0 * 2.0 / (f)))
#else
#define DC_SETTLE       0
#define DC_PEAK(peak, f) 0
#endif

static void TestSwitching(void)
{
    printf("switching on and off mid-stream is click-free\n");
    rxeq_t eq;
    const double freqs[] = { 300, 1200, 2400 };

    for (unsigned k = 0; k < sizeof(freqs) / sizeof(freqs[0]); k++) {
        double f = freqs[k];

        /* Always on, for the steady-state step size and peak of the equalised signal */
        Start(&eq, RXEQ_PROFILE_K5, FS);
        for (long n = 0; n < LEN; n++) outOn[n] = Step(&eq, RXEQ_PROFILE_K5, FS, Sine(f, 0.5, n));

        /* Off, switched on at 0.25 s, off at 1.25 s */
        const long on = FS / 4, off = FS + FS / 4;
        long bypassBad = 0, onBad = 0;
        uint32_t stFade = 0, stOn = 0, stOff = 0;
        Start(&eq, RXEQ_PROFILE_NONE, FS);
        for (long n = 0; n < LEN; n++) {
            uint32_t ctrl = (n >= on && n < off) ? RXEQ_PROFILE_K5 : RXEQ_PROFILE_NONE;
            int16_t x = Sine(f, 0.5, n);
            outSw[n] = Step(&eq, ctrl, FS, x);
            if (n < on + RXEQ_SETTLE_LEN && outSw[n] != x) bypassBad++;
            if (n >= off + RXEQ_FADE_LEN && outSw[n] != x) bypassBad++;
            /* Once faded in (and the DC stage settled), the output is the equalised signal */
            if (n >= on + RXEQ_SETTLE_LEN + RXEQ_FADE_LEN + DC_SETTLE && n < off && abs(outSw[n] - outOn[n]) > 4) onBad++;
            if (n == on + RXEQ_SETTLE_LEN + 10) stFade = RxEq_Status(&eq);
            if (n == off - 1) stOn = RxEq_Status(&eq);
            if (n == off + RXEQ_FADE_LEN + 10) stOff = RxEq_Status(&eq);
        }

        glitch_t in = Measure(outSw, 1, on);
        glitch_t eqd = Measure(outOn, FS / 4, LEN);
        int steadyStep = in.maxStep > eqd.maxStep ? in.maxStep : eqd.maxStep;
        int steadyPeak = in.peak > eqd.peak ? in.peak : eqd.peak;
        glitch_t g1 = Measure(outSw, on, on + FS / 8), g2 = Measure(outSw, off, off + FS / 8);
        /* For comparison, the step an abrupt switch from the input to the equalised signal
         * would make at the same point */
        int abrupt = abs(outOn[on + RXEQ_SETTLE_LEN] - Sine(f, 0.5, on + RXEQ_SETTLE_LEN - 1));
        printf("  %4.0f Hz: switching on: step %d, peak %d; off: step %d, peak %d (steady: step %d, peak %d; "
               "an abrupt switch: step %d)\n",
               f, g1.maxStep, g1.peak, g2.maxStep, g2.peak, steadyStep, steadyPeak, abrupt);
        CHECK(bypassBad == 0, "%.0f Hz: %ld samples not bit-exact while off", f, bypassBad);
        CHECK(onBad == 0, "%.0f Hz: %ld samples differ from the always-on filter once on", f, onBad);
        /* The crossfade blends the input with a delayed, equalised copy: its step can be a
         * little more than either signal's own, by at most the difference of the two over one
         * fade step */
        int bound = steadyStep + 2 * steadyPeak / RXEQ_FADE_LEN + 2;
        CHECK(g1.maxStep <= bound && g2.maxStep <= bound, "%.0f Hz: step when switching (bound %d)", f, bound);
        int peakBound = steadyPeak + 2 + DC_PEAK(steadyPeak, f);
        CHECK(g1.peak <= peakBound && g2.peak <= peakBound, "%.0f Hz: overshoot when switching (bound %d)", f, peakBound);
        CHECK(stFade == (RXEQ_STATUS_FADE_MASK), "%.0f Hz: status 0x%08X during the fade in", f, stFade);
        CHECK(stOn == (RXEQ_PROFILE_K5 | RXEQ_STATUS_ACTIVE_MASK), "%.0f Hz: status 0x%08X while on", f, stOn);
        CHECK(stOff == 0, "%.0f Hz: status 0x%08X after switching off", f, stOff);
    }

    /* Switching back during the silent settle cancels the change, bit-exact throughout; a
     * change during the crossfade waits for it to finish */
    {
        long bad = 0;
        Start(&eq, RXEQ_PROFILE_NONE, FS);
        for (long n = 0; n < FS / 2; n++) {
            uint32_t ctrl = (n >= 1000 && n < 1000 + RXEQ_SETTLE_LEN / 2) ? RXEQ_PROFILE_K5 : RXEQ_PROFILE_NONE;
            int16_t x = Sine(700, 0.5, n);
            if (Step(&eq, ctrl, FS, x) != x) bad++;
        }
        CHECK(bad == 0, "cancelled switch-on: %ld samples not bit-exact", bad);

        Start(&eq, RXEQ_PROFILE_NONE, FS);
        long offAt = 1000 + RXEQ_SETTLE_LEN + RXEQ_FADE_LEN / 2;   /* in the middle of the fade in */
        long last = -1;
        for (long n = 0; n < FS / 2; n++) {
            uint32_t ctrl = (n >= 1000 && n < offAt) ? RXEQ_PROFILE_K5 : RXEQ_PROFILE_NONE;
            int16_t x = Sine(700, 0.5, n);
            if (Step(&eq, ctrl, FS, x) != x) last = n;
        }
        /* Fade in to the end, then (seeing off) fade straight back out */
        long expect = 1000 + RXEQ_SETTLE_LEN + 2 * RXEQ_FADE_LEN;
        CHECK(last >= 0 && last < expect + 2, "switch-off during the fade in: last changed sample %ld, expected before %ld",
              last, expect + 2);
        CHECK(RxEq_Status(&eq) == 0, "status 0x%08X after the switch-off", RxEq_Status(&eq));
    }

    /* The recording rate changing away from 48 kHz mid-stream fades out to bypass */
    {
        long bad = 0;
        Start(&eq, RXEQ_PROFILE_K5, FS);
        for (long n = 0; n < FS / 2; n++) {
            uint32_t fs = n < 5000 ? FS : 16000;
            int16_t x = Sine(700, 0.5, n);
            if (Step(&eq, RXEQ_PROFILE_K5, fs, x) != x && n >= 5000 + RXEQ_FADE_LEN) bad++;
        }
        CHECK(bad == 0, "after a rate change: %ld samples not bypassed", bad);
    }
}

static void TestDc(void)
{
#ifndef RXEQ_NO_DC_STAGE
    printf("DC settles to 0 (the DC stage)\n");
#else
    printf("DC does not drift or blow up (without the DC stage)\n");
#endif
    rxeq_t eq;
    const int16_t levels[] = { -95, 1000, -20000, 32767, -32768 };
    for (unsigned k = 0; k < sizeof(levels) / sizeof(levels[0]); k++) {
        Start(&eq, RXEQ_PROFILE_K5, FS);
        /* The decimator and interpolator are periodic over M samples, and their branches do
         * not have exactly the same DC gain, so DC comes out with a small ripple at 6 kHz
         * (about -76 dB of the level the equaliser makes of it). So look at whole periods:
         * their mean, and each phase at 1 s against the same phase at 10 s */
        int lo = 32767, hi = -32768, peak = 0;
        int16_t early[RXEQ_PHASES], late[RXEQ_PHASES];
        double meanLate = 0;
        for (long n = 0; n < 10L * FS; n++) {
            int16_t y = Step(&eq, RXEQ_PROFILE_K5, FS, levels[k]);
            if (abs(y) > peak) peak = abs(y);
            if (n >= FS) {
                if (y < lo) lo = y;
                if (y > hi) hi = y;
            }
            if (n >= FS && n < FS + RXEQ_PHASES) early[n - FS] = y;
            if (n >= 10L * FS - RXEQ_PHASES) {
                late[n - (10L * FS - RXEQ_PHASES)] = y;
                meanLate += y / (double) RXEQ_PHASES;
            }
        }
        int drift = 0;
        for (int i = 0; i < RXEQ_PHASES; i++) if (abs(early[i] - late[i]) > drift) drift = abs(early[i] - late[i]);
        printf("  input %6d: after 1 s the output is %d to %d, mean %.2f at 10 s, drift %d; start-up peak %d\n",
               levels[k], lo, hi, meanLate, drift, peak);
        CHECK(drift <= 1, "input %d: output drifts by %d", levels[k], drift);
#ifndef RXEQ_NO_DC_STAGE
        /* About 1.9 Hz: a time constant of 85 ms, so after 1 s even 2.9 times full scale is
         * down to a fraction of an LSB; what is left is the ripple. With the low-pass taps
         * rounded to int16, the 8 branches' DC gains differ by up to about 2 parts in 10^4 */
        int ripple = (int) (fabs(2.9 * levels[k]) * 2e-4) + 1;
        CHECK(fabs(meanLate) <= 1.0, "input %d: settled output averages %.2f", levels[k], meanLate);
        CHECK(lo >= -ripple && hi <= ripple, "input %d: settled output %d to %d, more than the ripple (%d)",
              levels[k], lo, hi, ripple);
#else
        int late0 = (int) lrint(meanLate);
        /* Without the DC stage, all FIR: DC settles to the design's DC gain, -2.9 */
        long expect = lrint(-2.9 * levels[k]);
        if (expect > 32767) expect = 32767;
        if (expect < -32768) expect = -32768;
        CHECK(labs(late0 - expect) <= labs(expect) / 50 + 1, "input %d: settled output %d, expected about %ld",
              levels[k], late0, expect);
#endif
    }
}

static void TestSaturation(void)
{
    printf("saturation clamps and is counted\n");
    rxeq_t eq;

    /* A full-scale 40 Hz tone: the EQ boosts it well beyond full scale */
    Start(&eq, RXEQ_PROFILE_K5, FS);
    long wrong = 0, atRail = 0;
    double prev = 0;
    for (long n = 0; n < FS; n++) {
        int16_t y = Step(&eq, RXEQ_PROFILE_K5, FS, Sine(40, 1.0, n));
        if (y == 32767 || y == -32768) atRail++;
        /* A wrap would show as a jump from one rail towards the other */
        if (n > 0 && fabs(y - prev) > 20000) wrong++;
        prev = y;
    }
    uint32_t st = RxEq_Status(&eq);
    printf("  full-scale 40 Hz: %ld samples at the rails, %u counted, %ld wrapped; status 0x%08X\n",
           atRail, eq.clips, wrong, st);
    CHECK(wrong == 0, "%ld wrapped samples", wrong);
    CHECK(atRail > 0 && eq.clips == atRail, "rail samples %ld, counted %u", atRail, eq.clips);
    CHECK((st >> RXEQ_STATUS_CLIPS_OFFS) == eq.clips, "status clip count 0x%08X", st);

    /* The count sticks at 0xFFFF */
    for (long n = 0; n < 20L * FS; n++) Step(&eq, RXEQ_PROFILE_K5, FS, Sine(40, 1.0, n));
    CHECK(eq.clips == 0xFFFF, "clip counter should stick at 0xFFFF, is %u", eq.clips);
    CHECK((RxEq_Status(&eq) >> RXEQ_STATUS_CLIPS_OFFS) == 0xFFFF, "status 0x%08X", RxEq_Status(&eq));

    /* A new stream starts the count afresh */
    RxEq_Reset(&eq, RXEQ_PROFILE_K5, FS);
    CHECK(eq.clips == 0, "count not cleared by a reset");

    /* A 1 kHz tone at full scale: the EQ has a little gain there, under 0.5 dB, so it clips a
     * little but never wraps */
    Start(&eq, RXEQ_PROFILE_K5, FS);
    wrong = 0;
    for (long n = 0; n < FS; n++) {
        int16_t x = Sine(1000, 1.0, n);
        int16_t y = Step(&eq, RXEQ_PROFILE_K5, FS, x);
        if (n > DELAY + 1000) {
            int16_t xd = Sine(1000, 1.0, n - DELAY);
            if ((xd > 16000 && y < 0) || (xd < -16000 && y > 0)) wrong++;
        }
    }
    CHECK(wrong == 0, "full-scale 1 kHz: %ld samples of the wrong sign", wrong);
}

static void TestCycles(void)
{
    printf("cycle statistics\n");
    rxeq_t eq;
    Start(&eq, RXEQ_PROFILE_K5, FS);
    for (uint32_t n = 0; n < 4096; n++) RxEq_CountCycles(&eq, n < 4095 ? 100 : 100 + 4096);
    uint32_t c = RxEq_Cycles(&eq);
    CHECK(c == ((101UL << RXEQ_CYCLES_AVG_OFFS) | 4196), "cycles word 0x%08X", c);
    RxEq_CountCycles(&eq, 1000000);
    CHECK((RxEq_Cycles(&eq) & 0xFFFF) == 0xFFFF, "max should stick at 0xFFFF");
    RxEq_Reset(&eq, RXEQ_PROFILE_K5, FS);
    CHECK(RxEq_Cycles(&eq) == 0, "not cleared by a reset");
}

static void TestGuard(void)
{
    printf("overload guard\n");
    rxeq_t eq;
    uint32_t passes = 0;

    /* Normal running: the main loop makes a pass every few samples, cycles in budget */
    Start(&eq, RXEQ_PROFILE_K5, FS);
    for (long n = 0; n < 2 * FS; n++) {
        Step(&eq, RXEQ_PROFILE_K5, FS, Sine(700, 0.5, n));
        if (n % 20 == 0) passes++;
        RxEq_Guard(&eq, 250, passes);
    }
    CHECK(RxEq_Status(&eq) == (RXEQ_PROFILE_K5 | RXEQ_STATUS_ACTIVE_MASK), "tripped in normal running: 0x%08X",
          RxEq_Status(&eq));

    /* Over budget now and then is fine; RXEQ_GUARD_RUN in a row trips it */
    for (long n = 0; n < FS; n++) {
        Step(&eq, RXEQ_PROFILE_K5, FS, Sine(700, 0.5, n));
        RxEq_Guard(&eq, (n % RXEQ_GUARD_RUN == 0) ? 100 : 5000, ++passes);
    }
    CHECK(!(RxEq_Status(&eq) & RXEQ_STATUS_OVERLOAD_MASK), "tripped by isolated runs");
    RxEq_Guard(&eq, 100, ++passes);     /* end the last run */
    long tripAt = -1;
    for (long n = 0; n < 1000; n++) {
        Step(&eq, RXEQ_PROFILE_K5, FS, Sine(700, 0.5, n));
        RxEq_Guard(&eq, RXEQ_GUARD_CYCLES + 1, ++passes);
        if (tripAt < 0 && (RxEq_Status(&eq) & RXEQ_STATUS_OVERLOAD_MASK)) tripAt = n;
    }
    uint32_t st = RxEq_Status(&eq);
    printf("  over budget every sample: tripped after %ld samples; status 0x%08X\n", tripAt + 1, st);
    CHECK(tripAt == RXEQ_GUARD_RUN - 1, "tripped after %ld samples, expected %d", tripAt + 1, RXEQ_GUARD_RUN);
    CHECK((st & (RXEQ_STATUS_ACTIVE_MASK | RXEQ_STATUS_FADE_MASK | RXEQ_STATUS_PROFILE_MASK)) == 0,
          "not off after tripping: 0x%08X", st);
    CHECK(((st & RXEQ_STATUS_OVERLOADS_MASK) >> RXEQ_STATUS_OVERLOADS_OFFS) == 1, "count 0x%08X", st);

    /* Off means off, bit for bit, even though the control register still asks for it */
    long bad = 0;
    for (long n = 0; n < FS; n++) {
        int16_t x = (int16_t) Rand();
        if (Step(&eq, RXEQ_PROFILE_K5, FS, x) != x) bad++;
        RxEq_Guard(&eq, 0, ++passes);
    }
    CHECK(bad == 0, "%ld samples changed after the guard tripped", bad);
    CHECK(RxEq_Status(&eq) & RXEQ_STATUS_OVERLOAD_MASK, "overload bit lost");

    /* A new recording lifts it; the count stays */
    RxEq_Reset(&eq, RXEQ_PROFILE_K5, FS);
    st = RxEq_Status(&eq);
    CHECK(st == (RXEQ_PROFILE_K5 | RXEQ_STATUS_ACTIVE_MASK | (1UL << RXEQ_STATUS_OVERLOADS_OFFS)),
          "after a reset: 0x%08X", st);

    /* The main loop starved: no pass for RXEQ_GUARD_STALE samples */
    tripAt = -1;
    for (long n = 0; n < 3 * RXEQ_GUARD_STALE; n++) {
        Step(&eq, RXEQ_PROFILE_K5, FS, Sine(700, 0.5, n));
        RxEq_Guard(&eq, 250, passes);
        if (tripAt < 0 && (RxEq_Status(&eq) & RXEQ_STATUS_OVERLOAD_MASK)) tripAt = n;
    }
    printf("  main loop starved: tripped after %ld samples (%.0f ms)\n", tripAt + 1, (tripAt + 1) * 1000.0 / FS);
    CHECK(tripAt == RXEQ_GUARD_STALE - 1, "tripped after %ld samples, expected %d", tripAt + 1, RXEQ_GUARD_STALE);
    CHECK(((RxEq_Status(&eq) & RXEQ_STATUS_OVERLOADS_MASK) >> RXEQ_STATUS_OVERLOADS_OFFS) == 2, "count");

    /* While bypassed the guard does nothing, and switching on after a long quiet main loop
     * (or none) does not trip at once */
    Start(&eq, RXEQ_PROFILE_NONE, FS);
    for (long n = 0; n < 10 * RXEQ_GUARD_STALE; n++) {
        Step(&eq, RXEQ_PROFILE_NONE, FS, 0);
        RxEq_Guard(&eq, 0, passes);
    }
    CHECK(!(RxEq_Status(&eq) & RXEQ_STATUS_OVERLOAD_MASK), "tripped while bypassed");
    for (long n = 0; n < RXEQ_GUARD_STALE / 2; n++) {
        Step(&eq, RXEQ_PROFILE_K5, FS, 0);
        RxEq_Guard(&eq, 0, passes);
    }
    CHECK(!(RxEq_Status(&eq) & RXEQ_STATUS_OVERLOAD_MASK), "tripped at once on switching on");

    /* The count sticks at 31 */
    for (int k = 0; k < 40; k++) {
        RxEq_Reset(&eq, RXEQ_PROFILE_K5, FS);
        for (int n = 0; n < RXEQ_GUARD_RUN; n++) {
            Step(&eq, RXEQ_PROFILE_K5, FS, 0);
            RxEq_Guard(&eq, 100000, ++passes);
        }
    }
    CHECK(((RxEq_Status(&eq) & RXEQ_STATUS_OVERLOADS_MASK) >> RXEQ_STATUS_OVERLOADS_OFFS) == 31, "count sticks at 31");
}

/* ------------------------------------------------------------------------------------------ */

/* The firmware runs the equaliser on blocks (RxEq_Poll once, then RxEq_ProcessBlock, then
 * RxEq_AccountBlock). The output must be bit for bit that of running it sample by sample, so
 * also the model's on the recording. With changes, the reference polls at the start of each
 * block too: a change that waits for a crossfade starts at the next poll, which in the
 * firmware is the next block (up to 1 ms later than with a poll at every sample) */
static int16_t blkIn[REF_LEN], blkRef[REF_LEN], blkOut[REF_LEN];

/* Control word over time: on, off during the settle (cancelled), on, a change during the
 * crossfade (waits), off, a reserved value, on */
static uint32_t Scenario(long n, long b)
{
    static const struct { long at; uint32_t ctrl; } ev[] = {
        { 0, RXEQ_PROFILE_NONE }, { 3000, RXEQ_PROFILE_K5 }, { 3500, RXEQ_PROFILE_NONE },
        { 6000, RXEQ_PROFILE_K5 }, { 6000 + RXEQ_SETTLE_LEN + 100, RXEQ_PROFILE_NONE },
        { 40000, RXEQ_PROFILE_K5 }, { 70000, 7 }, { 90000, RXEQ_PROFILE_K5 },
    };
    uint32_t ctrl = RXEQ_PROFILE_NONE;
    for (unsigned i = 0; i < sizeof(ev) / sizeof(ev[0]); i++) {
        if (n >= ev[i].at / b * b) ctrl = ev[i].ctrl;
    }
    return ctrl;
}

/* Two filters hold the same state: every field and delay line, the pointers as positions */
static int FiltSame(const rxeq_filter_t *a, const rxeq_filter_t *b)
{
    return a->phase == b->phase && a->xi == b->xi && a->loi == b->loi && a->yi == b->yi && a->di == b->di
        && a->fPart == b->fPart && a->xWin == b->xWin && a->acc == b->acc && a->dc == b->dc
        && (a->xRow - &a->x[0][0]) == (b->xRow - &b->x[0][0]) && (a->loNew - a->lo) == (b->loNew - b->lo)
        && (a->yNew - a->y) == (b->yNew - b->y)
        && !memcmp(a->x, b->x, sizeof(a->x)) && !memcmp(a->lo, b->lo, sizeof(a->lo))
        && !memcmp(a->y, b->y, sizeof(a->y)) && !memcmp(a->d, b->d, sizeof(a->d));
}

static void TestBlocks(void)
{
    printf("block processing is bit-exact with sample by sample\n");
    static const long sizes[] = { 48, 1, 7, 13, 22, 32, 47 };

    /* The recording from a reset, against the model */
    if (refIn[0] || refIn[1]) {
#ifndef RXEQ_NO_DC_STAGE
        long n2 = ReadRaw("data/rxeq_fixed_out_s16le.raw", refOut, REF_LEN);
#else
        long n2 = ReadRaw("data/rxeq_fixed_nodc_out_s16le.raw", refOut, REF_LEN);
#endif
        CHECK(n2 == REF_LEN, "model output missing");
        for (unsigned k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
            long b = sizes[k], len = REF_LEN / b * b, diff = 0;
            rxeq_t eq;
            Start(&eq, RXEQ_PROFILE_K5, FS);
            memcpy(blkOut, refIn, sizeof(refIn));
            for (long n = 0; n < len; n += b) {
                RxEq_Poll(&eq, RXEQ_PROFILE_K5, FS);
                RxEq_ProcessBlock(&eq, &blkOut[n], (uint32_t) b);
                RxEq_AccountBlock(&eq, 250 * (uint32_t) b, (uint32_t) b, (uint32_t) n);
            }
            for (long n = 0; n < len; n++) if (blkOut[n] != refOut[n]) diff++;
            printf("  recording in blocks of %2ld: %ld of %ld samples differ from the model\n", b, diff, len);
            CHECK(diff == 0, "blocks of %ld differ from the model", b);
            CHECK(RxEq_Status(&eq) == (RXEQ_PROFILE_K5 | RXEQ_STATUS_ACTIVE_MASK), "status 0x%08X",
                  RxEq_Status(&eq));
        }
    }

    /* Switching on and off, settle, crossfade, saturation: a loud bassy signal with noise */
    for (long n = 0; n < REF_LEN; n++) {
        double v = 0.45 * sin(2 * PI * 120.0 * n / FS) + 0.3 * sin(2 * PI * 1200.0 * n / FS)
                 + 0.2 * ((double) (Rand() & 0xFFFF) / 32768.0 - 1.0);
        long q = lrint(v * 32767);
        blkIn[n] = (int16_t) (q > 32767 ? 32767 : (q < -32768 ? -32768 : q));
    }
    for (unsigned k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
        long b = sizes[k], len = REF_LEN / b * b, diff = 0;
        rxeq_t ref, blk;
        Start(&ref, Scenario(0, b), FS);
        Start(&blk, Scenario(0, b), FS);
        uint32_t maxClips = 0, statusDiff = 0;
        for (long n = 0; n < len; n++) {
            if (n % b == 0) RxEq_Poll(&ref, Scenario(n, b), FS);
            blkRef[n] = RxEq_Process(&ref, blkIn[n]);
            uint32_t c = RxEq_Status(&ref) >> RXEQ_STATUS_CLIPS_OFFS;
            if (c > maxClips) maxClips = c;
        }
        memcpy(blkOut, blkIn, sizeof(blkIn));
        for (long n = 0; n < len; n += b) {
            RxEq_Poll(&blk, Scenario(n, b), FS);
            RxEq_ProcessBlock(&blk, &blkOut[n], (uint32_t) b);
        }
        for (long n = 0; n < len; n++) if (blkOut[n] != blkRef[n]) diff++;
        if (RxEq_Status(&blk) != RxEq_Status(&ref)) statusDiff++;
        if (!FiltSame(&blk.filt, &ref.filt)) statusDiff++;     /* and the whole filter state */
        printf("  switching in blocks of %2ld: %ld of %ld samples differ, status 0x%08X, %u clipped at most\n",
               b, diff, len, RxEq_Status(&blk), maxClips);
        CHECK(diff == 0 && statusDiff == 0, "blocks of %ld are not bit-exact", b);
        CHECK(maxClips > 0, "the scenario does not exercise saturation");
    }

    /* Bypass in blocks: untouched */
    {
        rxeq_t eq;
        Start(&eq, RXEQ_PROFILE_NONE, FS);
        memcpy(blkOut, blkIn, sizeof(blkIn));
        for (long n = 0; n + 48 <= REF_LEN; n += 48) {
            RxEq_Poll(&eq, RXEQ_PROFILE_NONE, FS);
            RxEq_ProcessBlock(&eq, &blkOut[n], 48);
        }
        CHECK(memcmp(blkOut, blkIn, sizeof(blkIn)) == 0, "bypass in blocks changed the signal");
    }
}

static void TestBlockAccounting(void)
{
    printf("cycle statistics and overload guard per block\n");
    rxeq_t eq;
    uint32_t passes = 0;
    int16_t blk[48];
    memset(blk, 0, sizeof(blk));

    /* Statistics: the average per sample over 4096 samples or more, the maximum the most
     * expensive block's average */
    Start(&eq, RXEQ_PROFILE_K5, FS);
    for (int k = 0; k < 85; k++) RxEq_CountBlock(&eq, 300 * 48, 48);
    CHECK((RxEq_Cycles(&eq) >> RXEQ_CYCLES_AVG_OFFS) == 0, "average before 4096 samples");
    RxEq_CountBlock(&eq, 300 * 48 + 86 * 48, 48);
    uint32_t c = RxEq_Cycles(&eq);
    CHECK((c >> RXEQ_CYCLES_AVG_OFFS) == 301 && (c & 0xFFFF) == 386, "cycles word 0x%08X", c);
    RxEq_CountBlock(&eq, 100000000, 48);
    CHECK((RxEq_Cycles(&eq) & 0xFFFF) == 0xFFFF, "max should stick at 0xFFFF");

    /* One block over budget trips it; exactly on budget does not */
    Start(&eq, RXEQ_PROFILE_K5, FS);
    for (int k = 0; k < 1000; k++) {
        RxEq_Poll(&eq, RXEQ_PROFILE_K5, FS);
        RxEq_ProcessBlock(&eq, blk, 48);
        RxEq_AccountBlock(&eq, RXEQ_GUARD_CYCLES * 48, 48, ++passes);
    }
    CHECK(!(RxEq_Status(&eq) & RXEQ_STATUS_OVERLOAD_MASK), "tripped on budget");
    RxEq_ProcessBlock(&eq, blk, 48);
    RxEq_AccountBlock(&eq, RXEQ_GUARD_CYCLES * 48 + 1, 48, ++passes);
    CHECK(RxEq_Status(&eq) & RXEQ_STATUS_OVERLOAD_MASK, "one block over budget did not trip it");

    /* Smaller blocks over budget trip it after RXEQ_GUARD_RUN samples (here 1 ms of blocks of 16) */
    Start(&eq, RXEQ_PROFILE_K5, FS);
    long tripAt = -1;
    for (int k = 0; k < 10; k++) {
        RxEq_ProcessBlock(&eq, blk, 16);
        RxEq_AccountBlock(&eq, (RXEQ_GUARD_CYCLES + 1) * 16, 16, ++passes);
        if (tripAt < 0 && (RxEq_Status(&eq) & RXEQ_STATUS_OVERLOAD_MASK)) tripAt = k;
    }
    CHECK(tripAt == RXEQ_GUARD_RUN / 16 - 1, "blocks of 16 over budget: tripped after %ld blocks", tripAt + 1);

    /* The main loop starved: 50 blocks of 48 (50 ms) without a pass, after one that saw it */
    Start(&eq, RXEQ_PROFILE_K5, FS);
    RxEq_AccountBlock(&eq, 250 * 48, 48, ++passes);
    tripAt = -1;
    for (int k = 0; k < 100; k++) {
        RxEq_ProcessBlock(&eq, blk, 48);
        RxEq_AccountBlock(&eq, 250 * 48, 48, passes);
        if (tripAt < 0 && (RxEq_Status(&eq) & RXEQ_STATUS_OVERLOAD_MASK)) tripAt = k;
    }
    printf("  main loop starved: tripped after %ld blocks of 48\n", tripAt + 1);
    CHECK(tripAt == RXEQ_GUARD_STALE / 48 - 1, "tripped after %ld blocks, expected %d", tripAt + 1,
          RXEQ_GUARD_STALE / 48);

    /* While bypassed it does nothing */
    Start(&eq, RXEQ_PROFILE_NONE, FS);
    for (int k = 0; k < 1000; k++) {
        RxEq_ProcessBlock(&eq, blk, 48);
        RxEq_AccountBlock(&eq, 100000, 48, passes);
    }
    CHECK(!(RxEq_Status(&eq) & RXEQ_STATUS_OVERLOAD_MASK), "tripped while bypassed");
}

int main(void)
{
    TestReference();
    TestBypass();
    TestSwitching();
    TestDc();
    TestSaturation();
    TestCycles();
    TestGuard();
    TestBlocks();
    TestBlockAccounting();
    printf(failures ? "\n%d FAILED\n" : "\nall tests passed\n", failures);
    return failures ? 1 : 0;
}
