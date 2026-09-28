#include "tx_eq.h"
#include <string.h>

enum {
    PHASE_IDLE,
    PHASE_SETTLE,
    PHASE_FADE,
};

void TxEq_Init(txeq_t *eq)
{
    memset(eq, 0, sizeof(*eq));
}

static uint8_t TargetSections(uint32_t ctrl, uint32_t fs)
{
    uint32_t fsReq = (ctrl & TXEQ_CTRL_FS_MASK) >> TXEQ_CTRL_FS_OFFS;
    if ((fsReq != 0) && (fsReq != fs)) {
        /* Designed for another sample rate: bypass */
        return 0;
    }
    return (ctrl & TXEQ_CTRL_NSECT_MASK) >> TXEQ_CTRL_NSECT_OFFS;
}

static void Capture(txeq_t *eq, uint32_t ctrl, const volatile uint32_t *coefs)
{
    for (uint8_t k = 0; k < TXEQ_MAX_SECTIONS; k++) {
        const volatile uint32_t *c = &coefs[k * TXEQ_COEF_PER_SECT];
        eq->pend[k].b0 = (int32_t) c[0];
        eq->pend[k].b1 = (int32_t) c[1];
        eq->pend[k].b2 = (int32_t) c[2];
        eq->pend[k].a1 = (int32_t) c[3];
        eq->pend[k].a2 = (int32_t) c[4];
    }
    eq->pendCtrl = ctrl;
    eq->ctrl = ctrl;
}

/* Load the captured set into bank b and start it from the input history, as if every section
 * had been running with unity gain until now */
static void Install(txeq_t *eq, uint8_t b, uint32_t fs)
{
    txeq_bank_t *bank = &eq->bank[b];
    int32_t x1 = (int32_t) eq->h1 * (1 << TXEQ_SIG_SHIFT); /* multiply, not <<: defined for negatives */
    int32_t x2 = (int32_t) eq->h2 * (1 << TXEQ_SIG_SHIFT);

    memcpy(bank->coef, eq->pend, sizeof(bank->coef));
    bank->ctrl = eq->pendCtrl;
    bank->nsect = TargetSections(eq->pendCtrl, fs);

    for (uint8_t k = 0; k < TXEQ_MAX_SECTIONS; k++) {
        bank->state[k].x1 = x1;
        bank->state[k].x2 = x2;
        bank->state[k].y1 = x1;
        bank->state[k].y2 = x2;
        bank->state[k].e = 0;
    }
}

void TxEq_Reset(txeq_t *eq, uint32_t ctrl, const volatile uint32_t *coefs, uint32_t fs)
{
    eq->h1 = 0;
    eq->h2 = 0;
    Capture(eq, ctrl, coefs);
    Install(eq, eq->cur, fs);
    eq->pendNew = 0;
    eq->phase = PHASE_IDLE;
    eq->count = 0;
    eq->clips = 0;
}

void TxEq_Poll(txeq_t *eq, uint32_t ctrl, const volatile uint32_t *coefs, uint32_t fs)
{
    if (ctrl != eq->ctrl) {
        /* Commit: copy all coefficients between two samples. If a change is still fading in,
         * this set waits, replacing any other set that was waiting */
        Capture(eq, ctrl, coefs);
        eq->pendNew = 1;
    }

    if (eq->pendNew && (eq->phase == PHASE_IDLE)) {
        uint8_t next = eq->cur ^ 1;
        Install(eq, next, fs);
        eq->pendNew = 0;
        eq->clips = 0;

        if ((eq->bank[next].nsect == 0) && (eq->bank[eq->cur].nsect == 0)) {
            /* Bypass to bypass: nothing to fade */
            eq->cur = next;
        } else if (eq->bank[next].nsect == 0) {
            /* To bypass: the target is the input itself, no need to settle */
            eq->phase = PHASE_FADE;
            eq->count = 0;
        } else {
            eq->phase = PHASE_SETTLE;
            eq->count = 0;
        }
    }
}

static inline int32_t Clamp(txeq_t *eq, int64_t v, int32_t lo, int32_t hi)
{
    if (v > hi) {
        if (eq->clips != 0xFFFF) eq->clips++;
        return hi;
    }
    if (v < lo) {
        if (eq->clips != 0xFFFF) eq->clips++;
        return lo;
    }
    return (int32_t) v;
}

/* Run one sample (internal scale) through a bank. A bank with no sections returns s. */
static int32_t RunBank(txeq_t *eq, txeq_bank_t *bank, int32_t s)
{
    for (uint8_t k = 0; k < bank->nsect; k++) {
        const txeq_coef_t *c = &bank->coef[k];
        txeq_state_t *st = &bank->state[k];

        /* |coef| <= 2^31 and |signal| <= 2^29, so each product fits in 2^60 and the sum of
         * five, plus a remainder below 2^29, cannot overflow 64 bits */
        int64_t acc = st->e;
        acc += (int64_t) c->b0 * s;
        acc += (int64_t) c->b1 * st->x1;
        acc += (int64_t) c->b2 * st->x2;
        acc -= (int64_t) c->a1 * st->y1;
        acc -= (int64_t) c->a2 * st->y2;

        /* Floor, and carry the remainder into the next sample (first-order error feedback) */
        int64_t yFull = acc >> TXEQ_COEF_FRAC;
        st->e = (int32_t) (acc - yFull * ((int64_t) 1 << TXEQ_COEF_FRAC));

        int32_t y = Clamp(eq, yFull, -TXEQ_SIG_LIMIT, TXEQ_SIG_LIMIT - 1);
        if (y != yFull) {
            st->e = 0;
        }

        st->x2 = st->x1;
        st->x1 = s;
        st->y2 = st->y1;
        st->y1 = y;
        s = y;
    }

    return s;
}

int16_t TxEq_Process(txeq_t *eq, int16_t x)
{
    txeq_bank_t *cur = &eq->bank[eq->cur];
    int32_t s = (int32_t) x * (1 << TXEQ_SIG_SHIFT);
    int32_t y;

    if (eq->phase == PHASE_IDLE) {
        if (cur->nsect == 0) {
            /* Bypass: untouched */
            eq->h2 = eq->h1;
            eq->h1 = x;
            return x;
        }
        y = RunBank(eq, cur, s);
    } else {
        txeq_bank_t *next = &eq->bank[eq->cur ^ 1];
        int32_t yOld = RunBank(eq, cur, s);
        int32_t yNew = RunBank(eq, next, s);

        if (eq->phase == PHASE_SETTLE) {
            /* The new set runs unheard until its start-up transient has died away */
            y = yOld;
            if (++eq->count >= TXEQ_SETTLE_LEN) {
                eq->phase = PHASE_FADE;
                eq->count = 0;
            }
        } else {
            eq->count++;
            y = yOld + (int32_t) (((int64_t) (yNew - yOld) * eq->count) >> TXEQ_FADE_SHIFT);
            if (eq->count >= TXEQ_FADE_LEN) {
                eq->cur ^= 1;
                eq->phase = PHASE_IDLE;
                eq->count = 0;
            }
        }
    }

    eq->h2 = eq->h1;
    eq->h1 = x;

    /* Round to int16 and saturate */
    int32_t out = (y + ((int32_t) 1 << (TXEQ_SIG_SHIFT - 1))) >> TXEQ_SIG_SHIFT;
    return (int16_t) Clamp(eq, out, -32768, 32767);
}

uint32_t TxEq_Status(const txeq_t *eq)
{
    const txeq_bank_t *cur = &eq->bank[eq->cur];
    return ((uint32_t) cur->nsect & TXEQ_STATUS_NSECT_MASK)
         | ((cur->nsect > 0) ? TXEQ_STATUS_ACTIVE_MASK : 0)
         | ((eq->phase != PHASE_IDLE) ? TXEQ_STATUS_FADE_MASK : 0)
         | (cur->ctrl & TXEQ_STATUS_GEN_MASK)
         | ((uint32_t) eq->clips << TXEQ_STATUS_CLIPS_OFFS);
}
