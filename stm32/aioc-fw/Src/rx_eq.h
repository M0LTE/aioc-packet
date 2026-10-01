#ifndef RX_EQ_H_
#define RX_EQ_H_

/*
 * Receive (recording) equaliser: undoes the UV-K5's receive audio high-pass (second order,
 * about 128 Hz, Q 0.5), magnitude and phase, so 9600 baud FSK decodes. Runs at 48 kHz only,
 * on the ADC's blocks of 1 ms (RxEq_ProcessBlock), the output bit for bit that of running it
 * sample by sample.
 *
 * This file and rx_eq.c have no hardware dependencies, so the same code is compiled on the
 * host for the unit tests in bench/test_rx_eq.c. bench/rxeq_fixed.py is a bit-exact model of
 * the arithmetic.
 *
 * Structure (the design is uvk5-packet-bench tools/rxeq_mr.py with a 97-tap low-pass; the
 * coefficients are in rx_eq_k5.h, in fixed point in rx_eq_k5_fixed.h):
 *
 *     y[n] = DC( x[n - D] + up( F( down(x) ) )[n] )
 *
 * The correction only matters at low frequencies, so it runs at fs / M (M = 8, 6 kHz):
 *   - down: a 97-tap linear-phase low-pass, evaluated once every M samples
 *   - F:    a 61-tap FIR at 6 kHz, the truncated inverse of the high-pass, minus 1
 *   - up:   zero-stuffing by M and the same low-pass, as a polyphase interpolator (gain M)
 *   - D:    336 samples (7.0 ms), the delay of the correction path, so the two line up
 *   - DC:   a first-order high-pass at about 1.9 Hz on the result (not in the design)
 *
 * The result follows the inverse of the high-pass closely from about 300 Hz up and roughly
 * down to 100 Hz (F is truncated to +-5 ms); below that the boost levels off at about +9 dB.
 * Without the DC stage the gain at DC would be -2.9, turning the ADC's own offset (around
 * -100 LSB on the bench AIOC) into about +275 LSB; the DC stage takes that out. Its time
 * constant is 85 ms, far below anything the equaliser corrects. The rest is FIR.
 *
 * Fixed point (the float version cost too much CPU for full duplex):
 *   - coefficient tables int16: the low-pass round(h 2^18), F round(f 2^16)
 *   - the decimator multiplies the low-pass with the int16 input two taps per instruction
 *     (SMLALD, 64-bit sum); the decimated signal keeps 13 fractional bits (int32)
 *   - F and the interpolator multiply 32-bit signals by 16-bit taps, keeping the top 32 bits
 *     of each product (SMLAWB and SMLAWT, two taps per coefficient load); F's output keeps
 *     13 fractional bits, the interpolator's (with the direct path added) 12, held within 4
 *     times full scale
 *   - the DC stage is a leaky integrator with a 64-bit state (no dead zone)
 *   - the output is rounded (half up) and saturated to int16
 * No intermediate can overflow even with full-scale input (the bounds are in rx_eq.c). The
 * response is within -72 dB of the float design (rxeq_fixed.py response).
 *
 * The work is spread evenly over the M samples of each decimation period: each sample runs
 * one polyphase branch of the decimator (13 taps, padded to 14 for the pairs) into a 64-bit
 * accumulator, a slice of F (9 taps, the taps that only need past decimated values) and one
 * branch of the interpolator (13 taps): 35 multiply-accumulates in 29 instructions. Every
 * loop has a fixed length and is unrolled.
 *
 * The coefficients are copied into RAM by RxEq_Init and RxEq_Reset, so the code, which runs
 * from flash, never reads data from flash in the loops (that stalls the flash prefetch).
 *
 * Switching: the control word picks a profile (0 bypass, 1 the K5 profile, anything else is
 * reserved and bypasses). Switching on: the filter runs unheard for RXEQ_SETTLE_LEN samples,
 * long enough to flush every delay line, so its output no longer depends on what was in
 * them; then the output crossfades from the input to the equalised signal over
 * RXEQ_FADE_LEN samples. Switching off crossfades back to the input. The equalised signal
 * lags the input by D, so the crossfade blends two time-shifted copies for 10.7 ms: no step,
 * no click. The DC stage starts from rest when switching on, so for a few hundred ms the
 * output carries a slow offset dying away: far below audio, and no step. In bypass (and not
 * fading) the input comes out bit for bit and the filter does not run.
 *
 * Overload guard (RxEq_Guard): if the equaliser's cost ever starves the rest of the firmware,
 * it switches itself off at once (no crossfade) and stays off until the next recording
 * starts, and the status register says so. It trips when the main loop has made no pass for
 * RXEQ_GUARD_STALE samples while the filter runs (the main loop refreshes the watchdog), or
 * when the filter took more than RXEQ_GUARD_CYCLES for RXEQ_GUARD_RUN samples in a row.
 */

#include <stdint.h>

#define RXEQ_FS             48000   /* the only rate the profile is designed for */
#define RXEQ_SETTLE_LEN     1024    /* samples, 21 ms at 48 kHz */
#define RXEQ_FADE_SHIFT     9
#define RXEQ_FADE_LEN       (1 << RXEQ_FADE_SHIFT)  /* samples, 10.7 ms at 48 kHz */

/* Overload guard */
#define RXEQ_GUARD_STALE    2400    /* samples (50 ms) without a main-loop pass */
#define RXEQ_GUARD_CYCLES   600     /* cycles per sample for the equaliser ... */
#define RXEQ_GUARD_RUN      48      /* ... this many samples in a row (1 ms) */

/* Profiles, as written to the control word (settings register SETTINGS_REG_RXEQ_CTRL) */
#define RXEQ_PROFILE_NONE   0
#define RXEQ_PROFILE_K5     1

/* Status word fields, identical to settings register SETTINGS_REG_INFO_RXEQ */
#define RXEQ_STATUS_PROFILE_MASK    0x000000FFUL
#define RXEQ_STATUS_ACTIVE_MASK     0x00000100UL
#define RXEQ_STATUS_FADE_MASK       0x00000200UL
#define RXEQ_STATUS_OVERLOAD_MASK   0x00000400UL    /* switched off by the guard, until recording restarts */
#define RXEQ_STATUS_OVERLOADS_OFFS  11              /* times the guard tripped since power-up, sticks at 31 */
#define RXEQ_STATUS_OVERLOADS_MASK  0x0000F800UL
#define RXEQ_STATUS_CLIPS_OFFS      16

/* Cycle statistics fields, identical to settings register SETTINGS_REG_INFO_RXEQCYC */
#define RXEQ_CYCLES_MAX_OFFS        0
#define RXEQ_CYCLES_AVG_OFFS        16
#define RXEQ_CYCLES_AVG_SHIFT       12      /* average over 4096 samples */

/* Sizes. rx_eq.c checks them against the coefficient tables in rx_eq_k5_fixed.h */
#define RXEQ_PHASES         8       /* M, the decimation factor */
#define RXEQ_BRANCH         13      /* taps per polyphase branch of the low-pass (97 / 8, rounded up) */
#define RXEQ_PAIRS          7       /* the decimator's branch in pairs of taps (padded to 14) */
#define RXEQ_XRING          14      /* input samples kept per phase (2 * RXEQ_PAIRS) */
#define RXEQ_F_SLICE        9       /* taps of F computed per sample between decimated samples */
#define RXEQ_F_SLICE_LEN    10      /* a slice padded to whole pairs */
#define RXEQ_F_LEN          64      /* F, padded with zeros to 1 + (M - 1) * RXEQ_F_SLICE taps */
#define RXEQ_LO_LEN         64      /* decimated input ring, for F (power of two, >= RXEQ_F_LEN - 1) */
#define RXEQ_D_LEN          336     /* direct path delay, D */

/* The rings are stored twice in a row and written backwards, so the window
 * ring[i .. i + taps - 1] always holds the newest samples first and needs no wrap-around.
 * Each phase's input ring is kept twice more, one sample further on, so that the window can
 * always be read as aligned pairs of int16 for the dual multiply-accumulate. The small state
 * comes first, where the Cortex-M4's short instructions can reach it */
#define RXEQ_XROW           (2 * (2 * RXEQ_XRING + 2))  /* int16 per phase: the ring, then the copy */
#define RXEQ_XODD           (2 * RXEQ_XRING + 2)        /* where the copy starts in the row */

typedef struct {
    uint32_t phase;         /* sample index modulo M; 0 completes a decimated sample */
    uint32_t xi;            /* ring positions (newest sample) */
    uint32_t loi, yi;
    uint32_t di;            /* direct path position */
    int32_t fPart;          /* next F output, all but its newest tap */
    int16_t *xRow;          /* &x[0][xi], and the offset of the aligned window in a row */
    uint32_t xWin;
    int32_t *loNew;         /* &lo[loi] */
    int32_t *yNew;          /* &y[yi] */
    int64_t acc;            /* decimator output being built up, one branch per sample */
    int64_t dc;             /* DC stage: 2^12 times the DC estimate */
    int16_t lp[RXEQ_PHASES][2 * RXEQ_PAIRS];        /* low-pass in polyphase order: lp[r][j] = h[r + M j] */
    int16_t fSlice[RXEQ_PHASES - 1][RXEQ_F_SLICE_LEN];  /* F taps 1 .. 63 in slices, padded with zeros */
    int16_t f0;                                     /* F tap 0 */
    int16_t x[RXEQ_PHASES][RXEQ_XROW] __attribute__((aligned(4)));  /* input, one row per phase: x[n], x[n - M], ... */
    int32_t lo[2 * RXEQ_LO_LEN];                    /* decimated input */
    int32_t y[2 * RXEQ_BRANCH];                     /* F output */
    int16_t d[RXEQ_D_LEN];                          /* direct path delay line */
} rxeq_filter_t;

typedef struct {
    uint8_t cur;            /* profile being heard (when idle) or faded from */
    uint8_t next;           /* profile being settled or faded to */
    uint8_t state;          /* 0 idle, 1 settling, 2 crossfading */
    uint8_t overload;       /* switched off by the guard until the next RxEq_Reset */
    uint8_t overloads;      /* times the guard tripped (sticks at 31; not cleared by RxEq_Reset) */
    uint16_t count;
    uint16_t clips;         /* saturated samples since the stream started or the last change
                             * (sticks at 0xFFFF) */
    uint16_t cycMax;        /* cycle statistics, see RxEq_CountCycles */
    uint16_t cycAvg;
    uint16_t cycN;          /* samples counted into cycSum */
    uint16_t overRun;       /* guard: samples in a row over RXEQ_GUARD_CYCLES */
    uint16_t stale;         /* guard: samples since the main loop last made a pass */
    uint32_t cycSum;
    uint32_t lastPasses;    /* guard: the main loop's pass counter when last seen */
    uint32_t lastCtrl;      /* RxEq_Poll: the control word and rate that last needed nothing done, */
    uint32_t lastFs;        /* to return at once if they have not changed (valid if settled) */
    uint8_t settled;
    rxeq_filter_t filt;
} rxeq_t;

/* Clear everything and load the coefficients; the equaliser starts in bypass */
void RxEq_Init(rxeq_t *eq);

/*
 * Start of a recording stream: clear all history and the statistics, (re)load the
 * coefficients into RAM, lift an overload bypass, and take the profile in ctrl straight away
 * (no crossfade, nothing is recording yet). fs is the recording rate in Hz; at any rate but
 * RXEQ_FS the equaliser bypasses.
 */
void RxEq_Reset(rxeq_t *eq, uint32_t ctrl, uint32_t fs);

/*
 * Call once per sample, before RxEq_Process. When the profile that ctrl and fs ask for
 * differs from the one in use, starts the settle and crossfade described above. A change
 * that arrives while a crossfade is running waits for it to finish; switching back during
 * the (silent) settle simply cancels it.
 */
void RxEq_Poll(rxeq_t *eq, uint32_t ctrl, uint32_t fs);

/* Filter one sample. In bypass (and not fading) returns x unchanged. */
int16_t RxEq_Process(rxeq_t *eq, int16_t x);

/* Filter a block of n samples in place, exactly as n calls of RxEq_Process. Call RxEq_Poll
 * once per block before it: a change then starts at a block boundary */
void RxEq_ProcessBlock(rxeq_t *eq, int16_t *x, uint32_t n);

/*
 * Overload guard: call once per sample, after RxEq_Process, with the cycles the equaliser
 * took and the main loop's pass counter (any counter the main loop increments on every pass).
 * Switches the equaliser off at once if it trips (see above). RxEq_GuardBlock does the same
 * once per block of n samples, with the cycles of the whole block.
 */
void RxEq_Guard(rxeq_t *eq, uint32_t cycles, uint32_t mainPasses);
void RxEq_GuardBlock(rxeq_t *eq, uint32_t cycles, uint32_t n, uint32_t mainPasses);

/* RxEq_CountCycles and RxEq_Guard in one call */
void RxEq_Account(rxeq_t *eq, uint32_t cycles, uint32_t mainPasses);

/* The same for a block of n samples that took cycles in all, for the interrupt: the cycles
 * per sample are the block's average, and the guard counts the block's samples (so with
 * blocks of 48 at 48 kHz it trips after the first block over budget, or 50 blocks without a
 * main-loop pass). With n = 1 this is RxEq_Account */
void RxEq_AccountBlock(rxeq_t *eq, uint32_t cycles, uint32_t n, uint32_t mainPasses);

/* Status word for the read-only info register */
uint32_t RxEq_Status(const rxeq_t *eq);

/*
 * Cycle statistics, for checking the CPU cost on hardware: the caller measures the
 * equaliser's time in each interrupt (DWT cycle counter) and passes it in here. Keeps the
 * maximum per sample since the last RxEq_Reset (sticks at 0xFFFF) and the average per sample
 * over each 4096 samples or more. RxEq_Cycles gives both as the info register's word.
 * RxEq_CountBlock takes the cycles of a block of n samples, and counts their average per
 * sample (so the maximum is the most expensive block's average).
 */
void RxEq_CountCycles(rxeq_t *eq, uint32_t cycles);
void RxEq_CountBlock(rxeq_t *eq, uint32_t cycles, uint32_t n);
uint32_t RxEq_Cycles(const rxeq_t *eq);

#endif /* RX_EQ_H_ */
