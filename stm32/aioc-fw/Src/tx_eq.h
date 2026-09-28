#ifndef TX_EQ_H_
#define TX_EQ_H_

/*
 * Transmit (playback) equaliser: a cascade of up to TXEQ_MAX_SECTIONS biquads in
 * direct form I, fixed point, run once per DAC sample.
 *
 * This file and tx_eq.c have no hardware dependencies, so the same code is compiled
 * on the host for the unit tests in bench/test_tx_eq.c.
 *
 * Formats (see bench/EQ.md):
 *   - coefficients: signed 32 bit, Q3.29 (1.0 = 1 << 29, range -4 .. +4)
 *   - y[n] = b0 x[n] + b1 x[n-1] + b2 x[n-2] - a1 y[n-1] - a2 y[n-2]  (a0 = 1)
 *   - internal signals: int16 sample * 2^TXEQ_SIG_SHIFT, clamped to +-TXEQ_SIG_LIMIT
 *     (12 dB of headroom between sections), accumulated in 64 bit, with first-order error
 *     feedback (the rounding remainder is carried into the next sample), which keeps poles
 *     near DC from holding a stuck offset or limit cycle
 *   - output rounded and saturated to int16
 *
 * Coefficient changes: a new set is copied from the registers in one go when the control
 * word changes. It then runs in the background from the current signal history for
 * TXEQ_SETTLE_LEN samples, so its start-up transient dies away unheard, and the output
 * crossfades from the old set to the new one over TXEQ_FADE_LEN samples. Enabling and
 * disabling go through the same crossfade (bypass counts as a set with no sections).
 */

#include <stdint.h>

#define TXEQ_MAX_SECTIONS   3
#define TXEQ_COEF_PER_SECT  5
#define TXEQ_NUM_COEFS      (TXEQ_MAX_SECTIONS * TXEQ_COEF_PER_SECT)
#define TXEQ_COEF_FRAC      29
#define TXEQ_SIG_SHIFT      12
#define TXEQ_SIG_LIMIT      ((int32_t) 1 << 29)
#define TXEQ_SETTLE_LEN     4096    /* samples, 85 ms at 48 kHz */
#define TXEQ_FADE_SHIFT     9
#define TXEQ_FADE_LEN       (1 << TXEQ_FADE_SHIFT)  /* samples, 10.7 ms at 48 kHz */

/* Control word fields, identical to settings register SETTINGS_REG_TXEQ_CTRL */
#define TXEQ_CTRL_NSECT_OFFS    0
#define TXEQ_CTRL_NSECT_MASK    0x00000003UL
#define TXEQ_CTRL_GEN_OFFS      8
#define TXEQ_CTRL_GEN_MASK      0x0000FF00UL
#define TXEQ_CTRL_FS_OFFS       16
#define TXEQ_CTRL_FS_MASK       0xFFFF0000UL

/* Status word fields, identical to settings register SETTINGS_REG_INFO_TXEQ */
#define TXEQ_STATUS_NSECT_MASK  0x00000003UL
#define TXEQ_STATUS_ACTIVE_MASK 0x00000004UL
#define TXEQ_STATUS_FADE_MASK   0x00000008UL
#define TXEQ_STATUS_GEN_MASK    0x0000FF00UL
#define TXEQ_STATUS_CLIPS_OFFS  16

typedef struct {
    int32_t b0, b1, b2, a1, a2;
} txeq_coef_t;

typedef struct {
    int32_t x1, x2, y1, y2;
    int32_t e;              /* rounding remainder carried to the next sample */
} txeq_state_t;

typedef struct {
    uint8_t nsect;          /* 0 = bypass */
    uint32_t ctrl;          /* control word this set was committed with */
    txeq_coef_t coef[TXEQ_MAX_SECTIONS];
    txeq_state_t state[TXEQ_MAX_SECTIONS];
} txeq_bank_t;

typedef struct {
    uint32_t ctrl;          /* last control word seen in the register */
    uint32_t pendCtrl;      /* last committed set, as copied at commit time */
    txeq_coef_t pend[TXEQ_MAX_SECTIONS];
    uint8_t pendNew;        /* committed set not yet started */
    uint8_t cur;            /* bank in use */
    uint8_t phase;          /* 0 idle, 1 settling, 2 crossfading */
    uint16_t count;
    uint16_t clips;         /* saturation events since the last commit (sticks at 0xFFFF) */
    int16_t h1, h2;         /* last two input samples */
    txeq_bank_t bank[2];
} txeq_t;

/* Clear everything; the equaliser starts in bypass */
void TxEq_Init(txeq_t *eq);

/*
 * Start of a playback stream: clear all history and install the set currently in the
 * registers straight away (no crossfade, there is nothing playing yet). ctrl is the control
 * word, coefs the TXEQ_NUM_COEFS coefficient words, fs the playback rate in Hz.
 */
void TxEq_Reset(txeq_t *eq, uint32_t ctrl, const volatile uint32_t *coefs, uint32_t fs);

/*
 * Call once per sample, before TxEq_Process. When ctrl differs from the last control word
 * seen, all coefficients are copied at once (between two samples), so a host can write the
 * coefficient registers in any order and then commit them with one write to the control
 * word. The new set takes over through the settle and crossfade described above.
 */
void TxEq_Poll(txeq_t *eq, uint32_t ctrl, const volatile uint32_t *coefs, uint32_t fs);

/* Filter one sample. In bypass (and not fading) returns x unchanged. */
int16_t TxEq_Process(txeq_t *eq, int16_t x);

/* Status word for the read-only info register */
uint32_t TxEq_Status(const txeq_t *eq);

#endif /* TX_EQ_H_ */
