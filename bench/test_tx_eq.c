/*
 * Host unit tests for the fixed-point TX equaliser (stm32/aioc-fw/Src/tx_eq.c).
 * Build and run: make -C bench test   (or see the Makefile next to this file)
 *
 * The firmware file is compiled unchanged, with the host gcc and the undefined-behaviour
 * sanitizer, so any signed overflow in the arithmetic aborts the test.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tx_eq.h"

#define FS 48000
#define PI 3.14159265358979323846

static int failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

/* The first coefficient set proposed by bench/eq.py for the measured K5 chain */
static const int32_t PROPOSAL[TXEQ_NUM_COEFS] = {
    +533154776, -1058790016, +525671785, -1058790016, +521955649,
    +314094285,  -258955703,  +78652127,  -550735937, +298405394,
    +539622902, -1006883977, +477329415, -1006883977, +480081404,
};

/* Register image as the firmware sees it: coefficients then control */
typedef struct {
    uint32_t coef[TXEQ_NUM_COEFS];
    uint32_t ctrl;
} regs_t;

static uint32_t Ctrl(int nsect, int gen, int fs)
{
    return (uint32_t) nsect | ((uint32_t) gen << 8) | ((uint32_t) fs << 16);
}

static void SetCoefs(regs_t *r, const int32_t *c, int n)
{
    for (int i = 0; i < n; i++) r->coef[i] = (uint32_t) c[i];
}

static int16_t Step(txeq_t *eq, const regs_t *r, uint32_t fs, int16_t x)
{
    TxEq_Poll(eq, r->ctrl, r->coef, fs);
    return TxEq_Process(eq, x);
}

/* Stream start with the registers as they are, like usb_audio.c does */
static void Start(txeq_t *eq, const regs_t *r, uint32_t fs)
{
    TxEq_Init(eq);
    TxEq_Reset(eq, r->ctrl, r->coef, fs);
}

static int32_t MaxResidue(const txeq_t *eq)
{
    int32_t m = 0;
    const txeq_bank_t *b = &eq->bank[eq->cur];
    for (int k = 0; k < b->nsect; k++) {
        int32_t v = abs(b->state[k].y1); if (v > m) m = v;
        v = abs(b->state[k].y2); if (v > m) m = v;
    }
    return m;
}

/* RBJ peaking section, quantised to Q3.29, for tests */
static void Peak(int32_t *out, double f0, double gdb, double q, double scale)
{
    double A = pow(10, gdb / 40), w = 2 * PI * f0 / FS, al = sin(w) / (2 * q), cw = cos(w);
    double a0 = 1 + al / A;
    double v[5] = { scale * (1 + al * A) / a0, scale * -2 * cw / a0, scale * (1 - al * A) / a0, -2 * cw / a0, (1 - al / A) / a0 };
    for (int i = 0; i < 5; i++) out[i] = (int32_t) llround(v[i] * (1 << TXEQ_COEF_FRAC));
}

/* Double-precision DF1 reference with the same quantised coefficients */
typedef struct { double x1, x2, y1, y2; } ref_state_t;

static double RefStep(const int32_t *c, int nsect, ref_state_t *st, double x)
{
    const double s = 1.0 / (1 << TXEQ_COEF_FRAC);
    for (int k = 0; k < nsect; k++) {
        const int32_t *q = &c[5 * k];
        double y = q[0] * s * x + q[1] * s * st[k].x1 + q[2] * s * st[k].x2 - q[3] * s * st[k].y1 - q[4] * s * st[k].y2;
        st[k].x2 = st[k].x1; st[k].x1 = x; st[k].y2 = st[k].y1; st[k].y1 = y;
        x = y;
    }
    return x;
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

/* ------------------------------------------------------------------------------------------ */

static void TestBypass(void)
{
    printf("bypass is bit-exact\n");
    txeq_t eq;
    regs_t r; memset(&r, 0, sizeof(r));
    long bad = 0;

    /* Defaults (all zero), both from power-up and after a stream start */
    TxEq_Init(&eq);
    for (long n = 0; n < 200000; n++) {
        int16_t x = (int16_t) Rand();
        if (n % 1000 == 0) x = (n % 2000) ? 32767 : -32768;
        if (Step(&eq, &r, FS, x) != x) bad++;
    }
    Start(&eq, &r, FS);
    for (long n = 0; n < 100000; n++) {
        int16_t x = (int16_t) Rand();
        if (Step(&eq, &r, FS, x) != x) bad++;
    }
    CHECK(bad == 0, "%ld samples changed with all registers zero", bad);

    /* Non-zero coefficients but NSECT 0: still untouched */
    SetCoefs(&r, PROPOSAL, TXEQ_NUM_COEFS);
    r.ctrl = Ctrl(0, 7, FS);
    for (long n = 0; n < 100000; n++) {
        int16_t x = (int16_t) Rand();
        if (Step(&eq, &r, FS, x) != x) bad++;
    }
    CHECK(bad == 0, "%ld samples changed with NSECT 0", bad);

    /* Designed for 48 kHz, stream at 22050: bypassed, whether committed during or before it */
    r.ctrl = Ctrl(3, 8, FS);
    for (long n = 0; n < 100000; n++) {
        int16_t x = (int16_t) Rand();
        if (Step(&eq, &r, 22050, x) != x) bad++;
    }
    Start(&eq, &r, 22050);
    for (long n = 0; n < 100000; n++) {
        int16_t x = (int16_t) Rand();
        if (Step(&eq, &r, 22050, x) != x) bad++;
    }
    CHECK(bad == 0, "%ld samples changed at a mismatched sample rate", bad);
    CHECK((TxEq_Status(&eq) & TXEQ_STATUS_ACTIVE_MASK) == 0, "status says active at a mismatched rate");

    /* Switched off again after use: after the fade-out it is bit-exact again */
    Start(&eq, &r, FS);
    for (long n = 0; n < FS; n++) Step(&eq, &r, FS, Sine(1000, 0.5, n));
    r.ctrl = 0;
    for (long n = 0; n < TXEQ_FADE_LEN + 1; n++) Step(&eq, &r, FS, (int16_t) Rand());
    for (long n = 0; n < 100000; n++) {
        int16_t x = (int16_t) Rand();
        if (Step(&eq, &r, FS, x) != x) bad++;
    }
    CHECK(bad == 0, "%ld samples changed after returning to bypass", bad);
    CHECK(TxEq_Status(&eq) == 0, "status 0x%08X after returning to bypass", TxEq_Status(&eq));
}

static void TestAccuracy(void)
{
    printf("fixed point matches the double-precision reference (proposal set, sines at -6 dBFS)\n");
    static const double freqs[] = { 20, 63, 100, 300, 1000, 2200, 3150, 5000, 6000, 12000 };
    const int nsect = 3;

    for (unsigned fi = 0; fi < sizeof(freqs) / sizeof(freqs[0]); fi++) {
        double f = freqs[fi];
        txeq_t eq;
        regs_t r; memset(&r, 0, sizeof(r));
        SetCoefs(&r, PROPOSAL, TXEQ_NUM_COEFS);
        r.ctrl = Ctrl(nsect, 1, FS);
        Start(&eq, &r, FS);
        ref_state_t st[3]; memset(st, 0, sizeof(st));

        long settle = FS, len = 2 * FS;
        double errPow = 0, refPow = 0, outPow = 0, inPow = 0;
        for (long n = 0; n < settle + len; n++) {
            int16_t x = Sine(f, 0.5, n);
            int16_t y = Step(&eq, &r, FS, x);
            double ref = RefStep(PROPOSAL, nsect, st, x);
            if (n >= settle) {
                errPow += (y - ref) * (y - ref);
                refPow += ref * ref;
                outPow += (double) y * y;
                inPow += (double) x * x;
            }
        }
        double snr = 10 * log10(refPow / errPow);
        double gainFixed = 10 * log10(outPow / inPow), gainRef = 10 * log10(refPow / inPow);
        double errRmsLsb = sqrt(errPow / len);
        printf("  %6.0f Hz: gain %+7.3f dB (reference %+7.3f), error %.3f LSB rms, SNR %.1f dB\n",
               f, gainFixed, gainRef, errRmsLsb, snr);
        CHECK(fabs(gainFixed - gainRef) < 0.01, "gain differs by %.4f dB at %.0f Hz", gainFixed - gainRef, f);
        /* Rounding to int16 alone gives 0.29 LSB rms. The AIOC's DAC keeps 12 of the 16 bits,
         * so anything under 1 LSB is far below what reaches the radio */
        CHECK(errRmsLsb < 0.5, "error %.3f LSB rms at %.0f Hz", errRmsLsb, f);
        CHECK(eq.clips == 0, "unexpected saturation at %.0f Hz", f);
    }
}

static void TestSaturation(void)
{
    printf("saturation clamps instead of wrapping\n");

    /* +12 dB peak at 1 kHz, no scaling: a full-scale 1 kHz sine must clip at the rails */
    {
        int32_t c[5]; Peak(c, 1000, 12, 1.0, 1.0);
        txeq_t eq;
        regs_t r; memset(&r, 0, sizeof(r));
        SetCoefs(&r, c, 5); r.ctrl = Ctrl(1, 1, FS);
        Start(&eq, &r, FS);
        ref_state_t st[1]; memset(st, 0, sizeof(st));
        long wrong = 0, atRail = 0;
        for (long n = 0; n < FS; n++) {
            int16_t x = Sine(1000, 1.0, n);
            int16_t y = Step(&eq, &r, FS, x);
            double ref = RefStep(c, 1, st, x);
            if (fabs(ref) > 33000 && ((ref > 0) != (y > 0))) wrong++;
            if (y == 32767 || y == -32768) atRail++;
        }
        printf("  output stage: %ld samples at the rails, %ld sign flips, clip count %u\n", atRail, wrong, eq.clips);
        CHECK(wrong == 0, "%ld wrapped samples", wrong);
        CHECK(atRail > 0 && eq.clips > 0, "no clipping recorded");
    }

    /* Internal clamp: sections 0 and 1 are flat x4 gains (the largest Q3.29 holds), 24 dB
     * against 12 dB of internal headroom, section 2 a flat /16. The clamp holds the signal at
     * 4x full scale, so the output must top out at a quarter of full scale with the right sign */
    {
        int32_t c[15] = { 0 };
        c[0] = INT32_MAX;
        c[5] = INT32_MAX;
        c[10] = (1 << TXEQ_COEF_FRAC) / 16;
        txeq_t eq;
        regs_t r; memset(&r, 0, sizeof(r));
        SetCoefs(&r, c, 15); r.ctrl = Ctrl(3, 1, FS);
        Start(&eq, &r, FS);
        long wrong = 0; int16_t ymax = 0;
        for (long n = 0; n < FS; n++) {
            int16_t x = Sine(700, 1.0, n);
            int16_t y = Step(&eq, &r, FS, x);
            if ((x > 20000 && y < 0) || (x < -20000 && y > 0)) wrong++;
            if (abs(y) > ymax) ymax = (int16_t) abs(y);
        }
        printf("  internal stage: peak out %d, %ld sign flips, clip count %u\n", ymax, wrong, eq.clips);
        CHECK(wrong == 0, "%ld wrapped samples", wrong);
        CHECK(eq.clips > 0, "internal clamp not counted");
        CHECK(ymax >= 8190 && ymax <= 8192, "internal clamp not at 4x full scale (peak %d)", ymax);
    }

    /* Worst case: every coefficient at the extremes, full-scale random input. Must not hit
     * undefined behaviour (the sanitizer aborts) and must stay within int16 */
    {
        txeq_t eq;
        regs_t r; memset(&r, 0, sizeof(r));
        for (int i = 0; i < TXEQ_NUM_COEFS; i++) r.coef[i] = (i % 2) ? 0x80000000u : 0x7FFFFFFFu;
        r.ctrl = Ctrl(3, 1, 0);
        Start(&eq, &r, FS);
        for (long n = 0; n < FS; n++) Step(&eq, &r, FS, (int16_t) Rand());
        printf("  extreme coefficients: survived, clip count %u\n", eq.clips);
        CHECK(eq.clips == 0xFFFF, "clip counter should stick at 0xFFFF, is %u", eq.clips);
    }
}

static void TestLimitCycles(void)
{
    printf("no limit cycles: silence after a signal decays to exact zero\n");
    int32_t sets[2][TXEQ_NUM_COEFS];
    memcpy(sets[0], PROPOSAL, sizeof(PROPOSAL));
    /* A nasty one: narrow, deep low-frequency notch and resonance */
    Peak(&sets[1][0], 25, -20, 8.0, 1.0);
    Peak(&sets[1][5], 40, +10, 6.0, 0.3);
    Peak(&sets[1][10], 20000, 6, 4.0, 0.5);

    for (int s = 0; s < 2; s++) {
        txeq_t eq;
        regs_t r; memset(&r, 0, sizeof(r));
        SetCoefs(&r, sets[s], TXEQ_NUM_COEFS); r.ctrl = Ctrl(3, 1, FS);
        Start(&eq, &r, FS);
        for (long n = 0; n < FS; n++) Step(&eq, &r, FS, (int16_t) (Rand() >> 17));
        long lastNonZero = -1;
        for (long n = 0; n < 20 * FS; n++) {
            if (Step(&eq, &r, FS, 0) != 0) lastNonZero = n;
        }
        int32_t residue = MaxResidue(&eq);
        printf("  set %d: last non-zero output %.3f s after the input stopped, internal residue %d (1 LSB = %d)\n",
               s, lastNonZero / (double) FS, residue, 1 << TXEQ_SIG_SHIFT);
        CHECK(lastNonZero < 5 * FS, "output still non-zero %.2f s after silence", lastNonZero / (double) FS);
        CHECK(residue < (1 << (TXEQ_SIG_SHIFT - 1)), "internal limit cycle of %d would reach the output", residue);
    }
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

#define LEN (3 * 48000)
static int16_t outA[LEN], outB[LEN], outC[LEN], outU[LEN], outX[LEN];

static void RunFixed(int16_t *out, const int32_t *c, double f)
{
    txeq_t eq;
    regs_t r; memset(&r, 0, sizeof(r));
    SetCoefs(&r, c, TXEQ_NUM_COEFS); r.ctrl = Ctrl(3, 1, FS);
    Start(&eq, &r, FS);
    for (long n = 0; n < LEN; n++) out[n] = Step(&eq, &r, FS, Sine(f, 0.5, n));
}

static void TestLiveUpdate(void)
{
    printf("live coefficient update is glitch-free\n");
    /* Filter A: the proposal. B and C: quite different 3-section sets */
    int32_t B[TXEQ_NUM_COEFS], C[TXEQ_NUM_COEFS];
    Peak(&B[0], 100, -8, 0.7, 0.5);
    Peak(&B[5], 4000, 4, 0.8, 1.0);
    Peak(&B[10], 800, -2, 1.0, 1.0);
    Peak(&C[0], 40, -12, 0.5, 1.0);
    Peak(&C[5], 2500, 3, 0.5, 0.7);
    Peak(&C[10], 300, 2, 2.0, 1.0);

    const double f = 1200;
    const long N = FS / 2;              /* commit of B */
    const long spw = 48;                /* samples per HID register write: one per millisecond at best */
    RunFixed(outA, PROPOSAL, f);
    RunFixed(outB, B, f);
    RunFixed(outC, C, f);

    /* Update run: A; the host writes B's 15 registers one per millisecond, in reverse order,
     * while audio keeps flowing; commits B; then, while B is still settling, writes C's words
     * (no commit yet: must not disturb B); then commits C once B has faded in */
    txeq_t eq;
    regs_t r; memset(&r, 0, sizeof(r));
    SetCoefs(&r, PROPOSAL, TXEQ_NUM_COEFS); r.ctrl = Ctrl(3, 1, FS);
    Start(&eq, &r, FS);
    long firstB = N - TXEQ_NUM_COEFS * spw, firstC = N + 10 * spw;
    const long M = N + FS;             /* commit of C */
    int wb = 0, wc = 0;
    int sawFadeFlag = 0;
    for (long n = 0; n < LEN; n++) {
        if (n >= firstB && (n - firstB) % spw == 0 && wb < TXEQ_NUM_COEFS) {
            int i = TXEQ_NUM_COEFS - 1 - wb++;
            r.coef[i] = (uint32_t) B[i];
        }
        if (n == N) r.ctrl = Ctrl(3, 2, FS);
        if (n >= firstC && (n - firstC) % spw == 0 && wc < TXEQ_NUM_COEFS) {
            int i = wc++;
            r.coef[i] = (uint32_t) C[i];
        }
        if (n == M) r.ctrl = Ctrl(3, 3, FS);
        outU[n] = Step(&eq, &r, FS, Sine(f, 0.5, n));
        if (n == N + 100 && (TxEq_Status(&eq) & TXEQ_STATUS_FADE_MASK)) sawFadeFlag = 1;
    }

    long diffBefore = 0;
    for (long n = 0; n < N + TXEQ_SETTLE_LEN; n++) if (outU[n] != outA[n]) diffBefore++;
    CHECK(diffBefore == 0, "%ld samples differ from A before the crossfade (a half-written set leaked)", diffBefore);
    CHECK(sawFadeFlag, "status did not show the change in progress");

    glitch_t steady = Measure(outA, FS / 4, LEN);
    glitch_t sb = Measure(outB, FS / 4, LEN), sc = Measure(outC, FS / 4, LEN);
    if (sb.maxStep > steady.maxStep) steady.maxStep = sb.maxStep;
    if (sc.maxStep > steady.maxStep) steady.maxStep = sc.maxStep;
    if (sb.peak > steady.peak) steady.peak = sb.peak;
    if (sc.peak > steady.peak) steady.peak = sc.peak;
    glitch_t atB = Measure(outU, N, N + FS / 4), atC = Measure(outU, M, M + FS / 4);

    long offB = 0, offC = 0;
    for (long n = N + FS / 4; n < M; n++) if (abs(outU[n] - outB[n]) > 1) offB++;
    for (long n = M + FS / 4; n < LEN; n++) if (abs(outU[n] - outC[n]) > 1) offC++;
    printf("  before the crossfade: %ld samples differ from A\n", diffBefore);
    printf("  A->B: biggest step %d, peak %d; B->C: biggest step %d, peak %d (steady signals: step %d, peak %d)\n",
           atB.maxStep, atB.peak, atC.maxStep, atC.peak, steady.maxStep, steady.peak);
    printf("  settled: %ld samples off B, %ld samples off C by more than 1 LSB "
           "(C's words written while B was settling did not leak)\n", offB, offC);
    CHECK(atB.maxStep <= steady.maxStep + 8 && atC.maxStep <= steady.maxStep + 8, "step at a change");
    CHECK(atB.peak <= steady.peak + 8 && atC.peak <= steady.peak + 8, "overshoot at a change");
    CHECK(offB == 0 && offC == 0, "did not settle to the committed set");

    /* For comparison: swapping coefficients abruptly (no settle, no crossfade) */
    {
        txeq_t e2;
        regs_t r2; memset(&r2, 0, sizeof(r2));
        SetCoefs(&r2, PROPOSAL, TXEQ_NUM_COEFS); r2.ctrl = Ctrl(3, 1, FS);
        Start(&e2, &r2, FS);
        for (long n = 0; n < N + FS / 4; n++) {
            if (n == N) {
                for (int k = 0; k < 3; k++) {
                    txeq_coef_t *c = &e2.bank[e2.cur].coef[k];
                    c->b0 = B[5 * k]; c->b1 = B[5 * k + 1]; c->b2 = B[5 * k + 2]; c->a1 = B[5 * k + 3]; c->a2 = B[5 * k + 4];
                }
            }
            outX[n] = Step(&e2, &r2, FS, Sine(f, 0.5, n));
        }
        glitch_t g = Measure(outX, N, N + FS / 4);
        printf("  (for comparison, an abrupt swap A->B: biggest step %d, peak %d)\n", g.maxStep, g.peak);
    }
}

static void TestEnableDisable(void)
{
    printf("enabling and disabling mid-stream\n");
    txeq_t eq;
    regs_t r; memset(&r, 0, sizeof(r));
    Start(&eq, &r, FS);
    SetCoefs(&r, PROPOSAL, TXEQ_NUM_COEFS);
    const long on = FS / 4, off = FS + FS / 4;
    long bad = 0;
    for (long n = 0; n < 2 * FS; n++) {
        if (n == on) r.ctrl = Ctrl(3, 5, FS);
        if (n == off) r.ctrl = Ctrl(0, 6, FS);
        int16_t x = Sine(1200, 0.5, n);
        outU[n] = Step(&eq, &r, FS, x);
        if (n < on + TXEQ_SETTLE_LEN && outU[n] != x) bad++;
        if (n >= off + TXEQ_FADE_LEN && outU[n] != x) bad++;
        if (n == on + TXEQ_SETTLE_LEN + TXEQ_FADE_LEN + 10) {
            uint32_t st = TxEq_Status(&eq);
            CHECK((st & 0xFF0F) == 0x0507, "status 0x%08X, expected GEN 5, active, NSECT 3", st);
        }
    }
    glitch_t steady = Measure(outU, 1, on);
    glitch_t g1 = Measure(outU, on, on + FS / 4), g2 = Measure(outU, off, off + FS / 4);
    printf("  bypass before and after: %ld samples not bit-exact; switching on: step %d, peak %d; "
           "off: step %d, peak %d (input: step %d, peak %d)\n",
           bad, g1.maxStep, g1.peak, g2.maxStep, g2.peak, steady.maxStep, steady.peak);
    CHECK(bad == 0, "bypass not bit-exact around the switch");
    CHECK(g1.maxStep <= steady.maxStep + 8 && g2.maxStep <= steady.maxStep + 8, "step when switching");
    CHECK(g1.peak <= steady.peak + 8 && g2.peak <= steady.peak + 8, "overshoot when switching");
}

int main(void)
{
    TestBypass();
    TestAccuracy();
    TestSaturation();
    TestLimitCycles();
    TestLiveUpdate();
    TestEnableDisable();
    printf(failures ? "\n%d FAILED\n" : "\nall tests passed\n", failures);
    return failures ? 1 : 0;
}
