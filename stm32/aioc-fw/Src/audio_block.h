#ifndef AUDIO_BLOCK_H_
#define AUDIO_BLOCK_H_

/*
 * Block processing for the audio path. The DMA moves samples between the converters and a
 * buffer of two halves, one block each; when the DMA moves on from one half, its interrupt
 * processes that half as one block of fs / 1000 samples (1 ms). These are the parts of that
 * which need no hardware: the sample format of the converters, the virtual COS and PTT level
 * check, the volume, which half is free, how much playback the DMA still holds, and the
 * cycle statistics.
 *
 * This file and audio_block.c have no hardware dependencies, so the same code is compiled
 * on the host for the unit tests in bench/test_audio_block.c.
 */

#include <stdint.h>

#define AUDIO_BLOCK_MAX     48      /* samples per block at the highest rate, 48 kHz */

/* Cycle statistics fields, identical to settings registers SETTINGS_REG_INFO_RXCYC and
 * SETTINGS_REG_INFO_TXCYC */
#define AUDIO_CYCLES_MAX_OFFS       0
#define AUDIO_CYCLES_AVG_OFFS       16
#define AUDIO_CYCLES_AVG_SAMPLES    4096    /* average over at least this many samples */

/* Samples per block at fs Hz: fs / 1000 (22 at 22050 Hz, 11 at 11025 Hz), within 1 and
 * AUDIO_BLOCK_MAX */
uint32_t AudioBlock_Len(uint32_t fs);

/* ADC data (12 bit, left aligned, so 0 to 65520 around 32768) to signed samples, and signed
 * samples to DAC data (left aligned), as the per-sample interrupts did */
void AudioBlock_FromAdc(int16_t *dst, const volatile uint16_t *src, uint32_t n);
void AudioBlock_ToDac(volatile uint16_t *dst, const int16_t *src, uint32_t n);

/* 1 if any sample is above threshold or below -threshold: the virtual COS and PTT check */
uint8_t AudioBlock_Loud(const int16_t *x, uint32_t n, uint16_t threshold);

/* Scale by a 16-bit unsigned volume (65535 is 1, and leaves every sample as it is), rounded
 * as the per-sample interrupts did */
void AudioBlock_Volume(int16_t *x, uint32_t n, uint16_t volume);

/*
 * A circular DMA buffer of two halves of n samples, 2n in all. remaining is the DMA's count
 * of transfers left before it wraps (CNDTR: 2n down to 1).
 *
 * AudioBlock_FreeHalf: the half the DMA is not in, so the one to fill or empty (0 or 1).
 *
 * AudioBlock_PlayPending: playback samples the DMA has yet to send to the DAC: the rest of
 * the half it is in, and the other half too if that was filled after the DMA left it (filled
 * is the half filled most recently). Called with the DMA interrupt held off, this is exact
 * even when the DMA has just crossed into the other half and its interrupt is still pending.
 */
uint32_t AudioBlock_FreeHalf(uint32_t remaining, uint32_t n);
uint32_t AudioBlock_PlayPending(uint32_t remaining, uint32_t n, uint8_t filled);

/*
 * Cycle statistics of the block interrupts, from the DWT cycle counter: the most cycles one
 * block took (since the last reset, sticks at 0xFFFF) and the average per block over the last
 * AUDIO_CYCLES_AVG_SAMPLES samples or more (whole blocks).
 */
typedef struct {
    uint16_t max;
    uint16_t avg;
    uint32_t sum;
    uint32_t blocks;
    uint32_t samples;
} audio_cycles_t;

void AudioCycles_Reset(audio_cycles_t *c);
void AudioCycles_Count(audio_cycles_t *c, uint32_t cycles, uint32_t n);
uint32_t AudioCycles_Word(const audio_cycles_t *c);

#endif /* AUDIO_BLOCK_H_ */
