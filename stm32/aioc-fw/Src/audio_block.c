#include "audio_block.h"

uint32_t AudioBlock_Len(uint32_t fs)
{
    uint32_t n = fs / 1000;

    if (n < 1) {
        n = 1;
    }
    if (n > AUDIO_BLOCK_MAX) {
        n = AUDIO_BLOCK_MAX;
    }
    return n;
}

void AudioBlock_FromAdc(int16_t *dst, const volatile uint16_t *src, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        dst[i] = (int16_t) (((int32_t) src[i] - 32768) & 0xFFFFU);
    }
}

void AudioBlock_ToDac(volatile uint16_t *dst, const int16_t *src, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        dst[i] = (uint16_t) (((int32_t) src[i] + 32768) & 0xFFFFU);
    }
}

uint8_t AudioBlock_Loud(const int16_t *x, uint32_t n, uint16_t threshold)
{
    for (uint32_t i = 0; i < n; i++) {
        if ((x[i] > threshold) || (x[i] < -threshold)) {
            return 1;
        }
    }
    return 0;
}

void AudioBlock_Volume(int16_t *x, uint32_t n, uint16_t volume)
{
    for (uint32_t i = 0; i < n; i++) {
        int16_t s = x[i];
        x[i] = (int16_t) (((int32_t) s * volume + (s > 0 ? 32768 : -32768)) / 65536);
    }
}

uint32_t AudioBlock_FreeHalf(uint32_t remaining, uint32_t n)
{
    /* remaining > n: the DMA is in the first half */
    return (remaining > n) ? 1 : 0;
}

uint32_t AudioBlock_PlayPending(uint32_t remaining, uint32_t n, uint8_t filled)
{
    uint32_t cur = (remaining > n) ? 0 : 1;             /* the half the DMA is in */
    uint32_t rest = (remaining > n) ? remaining - n : remaining;

    /* The half filled last is queued behind this one if it is the other half; if it is this
     * one, the DMA has just come into it and the other half is spent, not yet refilled */
    return rest + ((filled != cur) ? n : 0);
}

void AudioCycles_Reset(audio_cycles_t *c)
{
    c->max = 0;
    c->avg = 0;
    c->sum = 0;
    c->blocks = 0;
    c->samples = 0;
}

void AudioCycles_Count(audio_cycles_t *c, uint32_t cycles, uint32_t n)
{
    if (cycles > 0xFFFF) {
        cycles = 0xFFFF;
    }
    if (cycles > c->max) {
        c->max = (uint16_t) cycles;
    }
    c->sum += cycles;
    c->blocks++;
    c->samples += n;
    if (c->samples >= AUDIO_CYCLES_AVG_SAMPLES) {
        c->avg = (uint16_t) (c->sum / c->blocks);
        c->sum = 0;
        c->blocks = 0;
        c->samples = 0;
    }
}

uint32_t AudioCycles_Word(const audio_cycles_t *c)
{
    return ((uint32_t) c->max << AUDIO_CYCLES_MAX_OFFS)
         | ((uint32_t) c->avg << AUDIO_CYCLES_AVG_OFFS);
}
