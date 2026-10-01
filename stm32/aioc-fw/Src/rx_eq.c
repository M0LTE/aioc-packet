#include "rx_eq.h"
#include "rx_eq_k5_fixed.h"
#include <string.h>

/* The compiler's first scheduling pass hoists all the loads of a loop ahead of the
 * multiplies, runs out of registers and spills them to the stack; without it, each load sits
 * next to its multiply-accumulate */
#pragma GCC optimize ("no-schedule-insns")

/* The Cortex-M4's DSP instructions, one cycle each; the same arithmetic in C on the host:
 *   SMLALD  two 16 x 16 products (the halves of a and b) added to a 64-bit sum
 *   SMLAWB  a 32 x 16 product (b's bottom half), shifted down 16 (floor), added to a 32-bit sum
 *   SMLAWT  the same with b's top half
 *   SSAT30  saturate to 30 bits signed */
#if defined(__ARM_FEATURE_DSP)
#include <arm_acle.h>
#define SMLALD(a, b, acc)   __smlald((int32_t) (a), (int32_t) (b), (acc))
#define SMLAWB(a, b, acc)   __smlawb((a), (int32_t) (b), (acc))
#define SMLAWT(a, b, acc)   __smlawt((a), (int32_t) (b), (acc))
#define SSAT30(a)           __ssat((a), 30)
#else
static inline int64_t SMLALD(uint32_t a, uint32_t b, int64_t acc)
{
    return acc + (int32_t) (int16_t) (a & 0xFFFF) * (int16_t) (b & 0xFFFF)
               + (int32_t) (int16_t) (a >> 16) * (int16_t) (b >> 16);
}
static inline int32_t SMLAWB(int32_t a, uint32_t b, int32_t acc)
{
    return acc + (int32_t) (((int64_t) a * (int16_t) (b & 0xFFFF)) >> 16);
}
static inline int32_t SMLAWT(int32_t a, uint32_t b, int32_t acc)
{
    return acc + (int32_t) (((int64_t) a * (int16_t) (b >> 16)) >> 16);
}
static inline int32_t SSAT30(int32_t a)
{
    return (a > (1 << 29) - 1) ? (1 << 29) - 1 : ((a < -(1 << 29)) ? -(1 << 29) : a);
}
#endif

/* Two int16 as one 32-bit word, first in the low half (little-endian); compiles to one load */
static inline uint32_t Pair(const int16_t *p)
{
    uint32_t w;
    memcpy(&w, p, sizeof(w));
    return w;
}

/* Fixed-point formats (see rx_eq.h and bench/rxeq_fixed.py, which models this bit for bit).
 * Worst cases with full-scale input everywhere (rxeq_fixed.py response prints them); every
 * partial sum is bounded by the sum of the absolute values of its terms:
 *   decimator sum  1.2e10 (int64)     lo    3.8e8 (int32, 13 fractional bits)
 *   F sum, y       1.5e9  (int32, < 2^31 = 2.15e9, 13 fractional bits)
 *   interpolator with the direct path   1.2e9 (int32, 12 fractional bits), then held within
 *   2^29; DC stage state 2.2e12 (int64), its output within 2^30 */
#define LO_ROUND        (RXEQ_LP_SHIFT - 13)    /* decimator sum (2^-18) to lo (2^-13) */
#define X_SHIFT         12                      /* the direct path in v's units (2^-12) */
#define DC_SHIFT        12                      /* DC stage time constant 2^12 samples */
#define OUT_SHIFT       12                      /* v to samples */

/* Samples it takes for anything in the delay lines to have left every part of the filter:
 * the decimator, F (at the low rate) and the interpolator, plus one decimation period */
#define RXEQ_MEMORY     ((RXEQ_LP_TAPS - 1) + RXEQ_M * (RXEQ_F_TAPS - 1) + (RXEQ_LP_TAPS - 1) + RXEQ_M)

_Static_assert(RXEQ_PHASES == RXEQ_M, "rx_eq.h and rx_eq_k5_fixed.h disagree on M");
_Static_assert((RXEQ_M & (RXEQ_M - 1)) == 0, "M must be a power of two");
_Static_assert(RXEQ_BRANCH * RXEQ_M >= RXEQ_LP_TAPS && (RXEQ_BRANCH - 1) * RXEQ_M < RXEQ_LP_TAPS,
               "branch length must be the low-pass length / M, rounded up");
_Static_assert(2 * RXEQ_PAIRS >= RXEQ_BRANCH && RXEQ_XRING == 2 * RXEQ_PAIRS, "the pairs must cover a branch");
_Static_assert(RXEQ_F_LEN == 1 + (RXEQ_M - 1) * RXEQ_F_SLICE && RXEQ_F_LEN >= RXEQ_F_TAPS,
               "the slices of F must cover all its taps");
_Static_assert(RXEQ_LO_LEN >= RXEQ_F_LEN - 1 && (RXEQ_LO_LEN & (RXEQ_LO_LEN - 1)) == 0,
               "decimated history too short for F, or not a power of two");
_Static_assert(RXEQ_D_LEN == RXEQ_DELAY, "direct path delay differs from the coefficient tables");
_Static_assert(RXEQ_DELAY == (RXEQ_LP_TAPS - 1) + RXEQ_M * (RXEQ_F_TAPS - 1) / 2,
               "the direct path must line up with the correction path");
_Static_assert(RXEQ_SETTLE_LEN >= RXEQ_MEMORY, "settle too short to flush the delay lines");
_Static_assert(RXEQ_LP_SHIFT == 18 && RXEQ_F_SHIFT == 16, "the formats above assume these scales");
_Static_assert(RXEQ_F_SLICE_LEN >= RXEQ_F_SLICE && !(RXEQ_F_SLICE_LEN & 1), "slices in whole pairs");

enum {
    STATE_IDLE,
    STATE_SETTLE,
    STATE_FADE,
};

/* Copy the coefficients from flash into RAM: the low-pass in polyphase order, both padded
 * with zero taps, which add exactly nothing. Kept as loops (unrolled, gcc turns the copy into
 * a kilobyte of code) */
static __attribute__((noinline)) void LoadTables(rxeq_filter_t *f)
{
#pragma GCC unroll 1
    for (uint32_t r = 0; r < RXEQ_M; r++) {
#pragma GCC unroll 1
        for (uint32_t j = 0; j < 2 * RXEQ_PAIRS; j++) {
            uint32_t i = r + RXEQ_M * j;
            f->lp[r][j] = (i < RXEQ_LP_TAPS) ? rxeqLpQ[i] : 0;
        }
    }
#pragma GCC unroll 1
    for (uint32_t s = 0; s < RXEQ_M - 1; s++) {
#pragma GCC unroll 1
        for (uint32_t j = 0; j < RXEQ_F_SLICE_LEN; j++) {
            uint32_t i = 1 + s * RXEQ_F_SLICE + j;
            f->fSlice[s][j] = ((j < RXEQ_F_SLICE) && (i < RXEQ_F_TAPS)) ? rxeqFQ[i] : 0;
        }
    }
    f->f0 = rxeqFQ[0];
}

/* The pointers that follow the ring positions (they change only at phase 0) */
static void SetPointers(rxeq_filter_t *f)
{
    f->xRow = &f->x[0][f->xi];
    f->xWin = (f->xi & 1) * (RXEQ_XODD + 1);
    f->loNew = &f->lo[f->loi];
    f->yNew = &f->y[f->yi];
}

void RxEq_Init(rxeq_t *eq)
{
    memset(eq, 0, sizeof(*eq));
    LoadTables(&eq->filt);
    SetPointers(&eq->filt);
}

static uint8_t TargetProfile(const rxeq_t *eq, uint32_t ctrl, uint32_t fs)
{
    if (!eq->overload && (ctrl == RXEQ_PROFILE_K5) && (fs == RXEQ_FS)) {
        return RXEQ_PROFILE_K5;
    }
    /* Off, a reserved value, another sample rate, or switched off by the guard: bypass */
    return RXEQ_PROFILE_NONE;
}

void RxEq_Reset(rxeq_t *eq, uint32_t ctrl, uint32_t fs)
{
    memset(&eq->filt, 0, sizeof(eq->filt));
    LoadTables(&eq->filt);
    SetPointers(&eq->filt);
    eq->overload = 0;
    eq->settled = 0;
    eq->cur = TargetProfile(eq, ctrl, fs);
    eq->next = eq->cur;
    eq->state = STATE_IDLE;
    eq->count = 0;
    eq->clips = 0;
    eq->cycMax = 0;
    eq->cycAvg = 0;
    eq->cycSum = 0;
    eq->cycN = 0;
    eq->overRun = 0;
    eq->stale = 0;
}

void RxEq_Poll(rxeq_t *eq, uint32_t ctrl, uint32_t fs)
{
    if (eq->settled && (ctrl == eq->lastCtrl) && (fs == eq->lastFs)) {
        /* The usual case: nothing changed since it last needed nothing done */
        return;
    }

    uint8_t target = TargetProfile(eq, ctrl, fs);

    if ((eq->state == STATE_SETTLE) && (target == eq->cur)) {
        /* Switched back before anything was heard */
        eq->state = STATE_IDLE;
        eq->count = 0;
        return;
    }

    if (eq->state != STATE_IDLE) {
        /* A change during a crossfade waits for it to finish (so this is not remembered as
         * seen) */
        return;
    }

    if (target == eq->cur) {
        eq->lastCtrl = ctrl;
        eq->lastFs = fs;
        eq->settled = 1;
        return;
    }

    /* With one profile, a change is always on to off or off to on, so one filter is enough.
     * The filter was not run while bypassed; whatever is left in its delay lines is flushed
     * out during the settle, so it needs no clearing here (and the interrupt no memset).
     * The DC stage, which does not forget, starts from zero: it then only has to take out
     * the equaliser's own DC, which it does with its 85 ms time constant, unheard at first
     * and then far below audio */
    eq->settled = 0;
    eq->next = target;
    eq->count = 0;
    eq->clips = 0;
    if (target != RXEQ_PROFILE_NONE) {
        eq->filt.dc = 0;
        eq->state = STATE_SETTLE;
    } else {
        eq->state = STATE_FADE;
    }
}

/* One sample through the filter: x[n - D] + up(F(down(x)))[n], then the DC stage, in samples
 * (rounded, not yet saturated) */
static inline __attribute__((always_inline)) int32_t FilterRun(rxeq_filter_t *f, int16_t x)
{
    uint32_t p = f->phase & (RXEQ_M - 1);   /* the mask tells the compiler p < M */

    /* Direct path: read the sample from D ago, then put this one in its place */
    int32_t direct = f->d[f->di];
    f->d[f->di] = x;
    f->di = (f->di == RXEQ_D_LEN - 1) ? 0 : f->di + 1;

    /* Input, into this phase's ring and its copy one place on. All M rings step back
     * together, at phase 0, so xw[j] is x[n - M j], read from whichever copy has the window
     * starting at an even index, so in aligned pairs */
    if (p == 0) {
        f->xi = (f->xi == 0) ? RXEQ_XRING - 1 : f->xi - 1;
        f->xRow = &f->x[0][f->xi];
        f->xWin = (f->xi & 1) * (RXEQ_XODD + 1);
    }
    int16_t *xr = f->xRow + p * RXEQ_XROW;
    xr[0] = x;
    xr[RXEQ_XRING] = x;
    xr[RXEQ_XODD + 1] = x;
    xr[RXEQ_XODD + 1 + RXEQ_XRING] = x;
    const int16_t *xw = xr + f->xWin;

    /* Decimator: the decimated sample at the next multiple of M, n + r, needs the taps
     * r, r + M, r + 2M, ... applied to x[n], x[n - M], x[n - 2M], ... This sample's branch
     * goes into the accumulator, two taps per instruction; the branch with r = 0 completes it */
    const int16_t *h = f->lp[(RXEQ_M - p) & (RXEQ_M - 1)];
    int64_t acc = f->acc;
    for (uint32_t k = 0; k < RXEQ_PAIRS; k++) {
        acc = SMLALD(Pair(&h[2 * k]), Pair(&xw[2 * k]), acc);
    }

    if (p == 0) {
        /* New decimated sample: round to 13 fractional bits, store it, finish F with its
         * newest tap and store the result */
        int32_t lo = (int32_t) ((acc + (1 << (LO_ROUND - 1))) >> LO_ROUND);
        f->loi = (f->loi - 1) & (RXEQ_LO_LEN - 1);
        f->loNew = &f->lo[f->loi];
        f->loNew[0] = lo;
        f->loNew[RXEQ_LO_LEN] = lo;

        int32_t yLo = SMLAWB(lo, (uint16_t) f->f0, f->fPart);
        f->yi = (f->yi == 0) ? RXEQ_BRANCH - 1 : f->yi - 1;
        f->yNew = &f->y[f->yi];
        f->yNew[0] = yLo;
        f->yNew[RXEQ_BRANCH] = yLo;

        f->fPart = 0;
        acc = 0;
    } else {
        /* A slice of the next F output: taps 1 .. F_LEN - 1 on lo[k + 1 - j], k the newest
         * decimated sample. lw[i] is lo[k - i] */
        const int32_t *lw = f->loNew + (p - 1) * RXEQ_F_SLICE;
        const int16_t *c = f->fSlice[p - 1];
        int32_t fPart = f->fPart;
        for (uint32_t k = 0; k < RXEQ_F_SLICE / 2; k++) {
            uint32_t w = Pair(&c[2 * k]);
            fPart = SMLAWB(lw[2 * k], w, fPart);
            fPart = SMLAWT(lw[2 * k + 1], w, fPart);
        }
        fPart = SMLAWB(lw[RXEQ_F_SLICE - 1], Pair(&c[RXEQ_F_SLICE - 1]), fPart);
        f->fPart = fPart;
    }
    f->acc = acc;

    /* Interpolator: the zero-stuffed stream is nonzero only at multiples of M, so output n
     * needs only the taps p, p + M, p + 2M, ... applied to the newest F outputs. The gain of
     * M is in the units. The direct path goes in first */
    const int32_t *yw = f->yNew;
    h = f->lp[p];
    int32_t v = direct * (1 << X_SHIFT);
    for (uint32_t k = 0; k < RXEQ_BRANCH / 2; k++) {
        uint32_t w = Pair(&h[2 * k]);
        v = SMLAWB(yw[2 * k], w, v);
        v = SMLAWT(yw[2 * k + 1], w, v);
    }
    v = SMLAWB(yw[RXEQ_BRANCH - 1], Pair(&h[RXEQ_BRANCH - 1]), v);
    v = SSAT30(v);

    f->phase = (p + 1) & (RXEQ_M - 1);

#ifndef RXEQ_NO_DC_STAGE
    /* DC stage: a leaky integrator tracks the DC (2^12 times it, so no dead zone) */
    int32_t hp = v - (int32_t) (f->dc >> DC_SHIFT);
    f->dc += hp;
#else
    int32_t hp = v;
#endif
    return (hp + (1 << (OUT_SHIFT - 1))) >> OUT_SHIFT;
}

/* Saturate to int16, counting */
static inline __attribute__((always_inline)) int16_t Saturate(rxeq_t *eq, int32_t v)
{
    if (v > 32767) {
        if (eq->clips != 0xFFFF) eq->clips++;
        return 32767;
    }
    if (v < -32768) {
        if (eq->clips != 0xFFFF) eq->clips++;
        return -32768;
    }
    return (int16_t) v;
}

int16_t RxEq_Process(rxeq_t *eq, int16_t x)
{
    if ((eq->state == STATE_IDLE) && (eq->cur == RXEQ_PROFILE_NONE)) {
        /* Bypass: untouched, and the filter does not run */
        return x;
    }

    int32_t yEq = FilterRun(&eq->filt, x);

    if (eq->state == STATE_IDLE) {
        return Saturate(eq, yEq);
    }

    if (eq->state == STATE_SETTLE) {
        /* Switching on: the filter runs unheard until its delay lines hold only new input */
        if (++eq->count >= RXEQ_SETTLE_LEN) {
            eq->state = STATE_FADE;
            eq->count = 0;
        }
        return x;
    }

    /* Crossfade, from the equalised signal to the input or the other way round */
    int32_t from = (eq->cur != RXEQ_PROFILE_NONE) ? yEq : x;
    int32_t to = (eq->next != RXEQ_PROFILE_NONE) ? yEq : x;
    eq->count++;
    int32_t v = from + (int32_t) (((to - from) * (int32_t) eq->count) >> RXEQ_FADE_SHIFT);
    if (eq->count >= RXEQ_FADE_LEN) {
        eq->cur = eq->next;
        eq->state = STATE_IDLE;
        eq->count = 0;
    }
    return Saturate(eq, v);
}

void RxEq_ProcessBlock(rxeq_t *eq, int16_t *x, uint32_t n)
{
    if (eq->state == STATE_IDLE) {
        if (eq->cur == RXEQ_PROFILE_NONE) {
            /* Bypass: untouched, and the filter does not run */
            return;
        }
        /* Running and heard: only RxEq_Poll and the guard change that, between blocks */
        for (uint32_t i = 0; i < n; i++) {
            x[i] = Saturate(eq, FilterRun(&eq->filt, x[i]));
        }
        return;
    }

    /* Settling or crossfading */
    for (uint32_t i = 0; i < n; i++) {
        x[i] = RxEq_Process(eq, x[i]);
    }
}

/* Add n to a count that sticks at 0xFFFF */
static inline uint16_t AddSticky(uint16_t count, uint32_t n)
{
    return (count + n > 0xFFFF) ? 0xFFFF : (uint16_t) (count + n);
}

void RxEq_GuardBlock(rxeq_t *eq, uint32_t cycles, uint32_t n, uint32_t mainPasses)
{
    uint8_t running = !((eq->state == STATE_IDLE) && (eq->cur == RXEQ_PROFILE_NONE));

    if ((mainPasses != eq->lastPasses) || !running) {
        eq->lastPasses = mainPasses;
        eq->stale = 0;
    } else {
        eq->stale = AddSticky(eq->stale, n);
    }

    if (!running) {
        eq->overRun = 0;
        return;
    }

    if (cycles > RXEQ_GUARD_CYCLES * n) {
        eq->overRun = AddSticky(eq->overRun, n);
    } else {
        eq->overRun = 0;
    }

    if ((eq->stale >= RXEQ_GUARD_STALE) || (eq->overRun >= RXEQ_GUARD_RUN)) {
        /* Off at once: no crossfade, which would keep the filter running another 10.7 ms */
        eq->overload = 1;
        eq->settled = 0;
        if (eq->overloads < 31) eq->overloads++;
        eq->cur = RXEQ_PROFILE_NONE;
        eq->next = RXEQ_PROFILE_NONE;
        eq->state = STATE_IDLE;
        eq->count = 0;
        eq->overRun = 0;
        eq->stale = 0;
    }
}

void RxEq_Guard(rxeq_t *eq, uint32_t cycles, uint32_t mainPasses)
{
    RxEq_GuardBlock(eq, cycles, 1, mainPasses);
}

uint32_t RxEq_Status(const rxeq_t *eq)
{
    return ((uint32_t) eq->cur & RXEQ_STATUS_PROFILE_MASK)
         | ((eq->cur != RXEQ_PROFILE_NONE) ? RXEQ_STATUS_ACTIVE_MASK : 0)
         | ((eq->state != STATE_IDLE) ? RXEQ_STATUS_FADE_MASK : 0)
         | (eq->overload ? RXEQ_STATUS_OVERLOAD_MASK : 0)
         | (((uint32_t) eq->overloads << RXEQ_STATUS_OVERLOADS_OFFS) & RXEQ_STATUS_OVERLOADS_MASK)
         | ((uint32_t) eq->clips << RXEQ_STATUS_CLIPS_OFFS);
}

void RxEq_CountBlock(rxeq_t *eq, uint32_t cycles, uint32_t n)
{
    if (n == 0) {
        return;
    }
    if (cycles > 0xFFFFUL * n) {
        cycles = 0xFFFFUL * n;
    }
    uint32_t perSample = cycles / n;
    if (perSample > eq->cycMax) {
        eq->cycMax = (uint16_t) perSample;
    }
    /* Stays below 2^32 for blocks of up to a few thousand samples: 4096 + n samples of 0xFFFF */
    eq->cycSum += cycles;
    eq->cycN += n;
    if (eq->cycN >= (1U << RXEQ_CYCLES_AVG_SHIFT)) {
        eq->cycAvg = (uint16_t) (eq->cycSum / eq->cycN);
        eq->cycSum = 0;
        eq->cycN = 0;
    }
}

void RxEq_CountCycles(rxeq_t *eq, uint32_t cycles)
{
    RxEq_CountBlock(eq, cycles, 1);
}

void RxEq_AccountBlock(rxeq_t *eq, uint32_t cycles, uint32_t n, uint32_t mainPasses)
{
    RxEq_CountBlock(eq, cycles, n);
    RxEq_GuardBlock(eq, cycles, n, mainPasses);
}

void RxEq_Account(rxeq_t *eq, uint32_t cycles, uint32_t mainPasses)
{
    RxEq_AccountBlock(eq, cycles, 1, mainPasses);
}

uint32_t RxEq_Cycles(const rxeq_t *eq)
{
    return ((uint32_t) eq->cycMax << RXEQ_CYCLES_MAX_OFFS)
         | ((uint32_t) eq->cycAvg << RXEQ_CYCLES_AVG_OFFS);
}
