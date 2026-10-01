/*
 * Host unit tests for the audio block helpers (stm32/aioc-fw/Src/audio_block.c): the parts of
 * the DMA block processing that need no hardware.
 * Build and run: make -C bench test   (or see the Makefile next to this file)
 *
 * The per-sample expressions the interrupts used before the DMA rework are copied here as the
 * reference, and the block versions must match them exactly.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_block.h"

static int failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static uint32_t rngState = 12345;
static uint32_t Rand(void)
{
    rngState = rngState * 1664525u + 1013904223u;
    return rngState >> 8;
}

/* The per-sample interrupts' expressions */
static int16_t RefFromAdc(uint16_t dr)
{
    int16_t sample = ((int32_t) dr - 32768) & 0xFFFFU;
    return sample;
}

static uint16_t RefToDac(int16_t sample)
{
    return ((int32_t) sample + 32768) & 0xFFFFU;
}

static int16_t RefVolume(int16_t sample, uint16_t volume)
{
    return (int16_t) (((int32_t) sample * volume + (sample > 0 ? 32768 : -32768)) / 65536);
}

static int RefLoud(int16_t sample, uint16_t threshold)
{
    return (sample > threshold) || (sample < -threshold);
}

static void TestLen(void)
{
    printf("block length per rate\n");
    static const uint32_t rates[] = { 48000, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 96000, 500, 0 };
    static const uint32_t want[] = { 48, 32, 24, 22, 16, 12, 11, 8, AUDIO_BLOCK_MAX, 1, 1 };
    for (unsigned i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
        CHECK(AudioBlock_Len(rates[i]) == want[i], "%u Hz: %u samples, expected %u", rates[i],
              AudioBlock_Len(rates[i]), want[i]);
    }
}

static void TestConverters(void)
{
    printf("converters match the per-sample code for every value\n");
    static uint16_t raw[65536], back[65536];
    static int16_t s[65536];
    for (long i = 0; i < 65536; i++) raw[i] = (uint16_t) i;
    AudioBlock_FromAdc(s, raw, 65536);
    long bad = 0;
    for (long i = 0; i < 65536; i++) if (s[i] != RefFromAdc((uint16_t) i)) bad++;
    CHECK(bad == 0, "ADC: %ld values differ", bad);
    AudioBlock_ToDac(back, s, 65536);
    bad = 0;
    for (long i = 0; i < 65536; i++) if (back[i] != RefToDac(s[i]) || back[i] != raw[i]) bad++;
    CHECK(bad == 0, "DAC: %ld values differ", bad);
}

static void TestVolume(void)
{
    printf("volume matches the per-sample code for every sample\n");
    static const uint16_t volumes[] = { 0, 1, 2, 255, 32767, 32768, 46341, 65534, 65535 };
    static int16_t x[65536];
    for (unsigned v = 0; v < sizeof(volumes) / sizeof(volumes[0]) + 20; v++) {
        uint16_t vol = (v < sizeof(volumes) / sizeof(volumes[0])) ? volumes[v] : (uint16_t) Rand();
        for (long i = 0; i < 65536; i++) x[i] = (int16_t) (i - 32768);
        AudioBlock_Volume(x, 65536, vol);
        long bad = 0;
        for (long i = 0; i < 65536; i++) if (x[i] != RefVolume((int16_t) (i - 32768), vol)) bad++;
        CHECK(bad == 0, "volume %u: %ld samples differ", vol, bad);
        if (vol == 65535) {
            long changed = 0;
            for (long i = 0; i < 65536; i++) if (x[i] != (int16_t) (i - 32768)) changed++;
            CHECK(changed == 0, "volume 65535 changed %ld samples", changed);
        }
    }
}

static void TestLoud(void)
{
    printf("COS and PTT level check over a block: any sample past the threshold\n");
    static const uint16_t thresholds[] = { 0, 1, 100, 1000, 32766, 32767, 32768, 65535 };
    long bad = 0, hits = 0, blocks = 0;
    for (unsigned t = 0; t < sizeof(thresholds) / sizeof(thresholds[0]); t++) {
        uint16_t thr = thresholds[t];
        for (int trial = 0; trial < 20000; trial++) {
            int16_t x[AUDIO_BLOCK_MAX];
            uint32_t n = 1 + Rand() % AUDIO_BLOCK_MAX;
            int quiet = trial & 1;      /* mostly quiet blocks, with one sample near the threshold */
            for (uint32_t i = 0; i < n; i++) {
                x[i] = quiet ? (int16_t) ((int32_t) (Rand() % 201) - 100) : (int16_t) Rand();
            }
            if (quiet) {
                static const int offs[] = { -1, 0, 1 };
                int32_t v = (int32_t) thr + offs[Rand() % 3];
                if (Rand() & 1) v = -v;
                if (v > 32767) v = 32767;
                if (v < -32768) v = -32768;
                x[Rand() % n] = (int16_t) v;
            }
            int want = 0;
            for (uint32_t i = 0; i < n; i++) want |= RefLoud(x[i], thr);
            if (AudioBlock_Loud(x, n, thr) != want) bad++;
            hits += want;
            blocks++;
        }
    }
    printf("  %ld blocks, %ld loud\n", blocks, hits);
    CHECK(bad == 0, "%ld blocks judged differently", bad);
    CHECK(hits > blocks / 4 && hits < blocks, "the test blocks are not mixed enough");

    int16_t edge[3] = { 0, 0, 0 };
    CHECK(AudioBlock_Loud(edge, 3, 0) == 0, "silence is not loud at threshold 0");
    edge[2] = -1;
    CHECK(AudioBlock_Loud(edge, 3, 0) == 1, "-1 is loud at threshold 0");
    CHECK(AudioBlock_Loud(edge, 2, 0) == 0, "only the first n samples count");
    CHECK(AudioBlock_Loud(edge, 0, 0) == 0, "an empty block is quiet");
}

/* A circular DMA buffer of two halves, simulated transfer by transfer: the DMA's count
 * (CNDTR) runs 2n down to 1 and reloads; its interrupt comes when it has done half (count n)
 * and all (reload), and the handler runs `latency` transfers later. fresh[] marks samples
 * filled and not yet transferred, the truth AudioBlock_PlayPending must match */
static void TestDmaHalves(void)
{
    printf("DMA halves: the free half, and the playback still queued\n");
    long bad = 0, badHalf = 0, steps = 0;
    for (uint32_t n = 1; n <= AUDIO_BLOCK_MAX; n++) {
        for (uint32_t latency = 0; latency < n; latency += (n > 8 ? 3 : 1)) {
            uint8_t fresh[2 * AUDIO_BLOCK_MAX];
            uint32_t remaining = 2 * n;
            uint8_t filled;
            long pendingIrq = -1;       /* transfers until the handler runs, -1 none */
            uint32_t leftHalf = 0;      /* the half the DMA most recently left */

            /* Playback start: both halves filled, the DMA at the start */
            memset(fresh, 1, 2 * n);
            filled = 1;

            for (long t = 0; t < 40 * (long) n; t++) {
                /* The handler runs first if its time has come */
                if (pendingIrq == 0) {
                    uint32_t h = AudioBlock_FreeHalf(remaining, n);
                    if (h != leftHalf) badHalf++;
                    memset(&fresh[h * n], 1, n);
                    filled = (uint8_t) h;
                    pendingIrq = -1;
                }

                uint32_t truth = 0;
                for (uint32_t i = 0; i < 2 * n; i++) truth += fresh[i];
                if (AudioBlock_PlayPending(remaining, n, filled) != truth) bad++;
                steps++;

                /* One transfer */
                uint32_t pos = 2 * n - remaining;
                if (!fresh[pos]) bad++;  /* it would play a stale sample: an underrun */
                fresh[pos] = 0;
                remaining--;
                if (remaining == n) {
                    leftHalf = 0;
                    pendingIrq = latency;
                } else if (remaining == 0) {
                    remaining = 2 * n;
                    leftHalf = 1;
                    pendingIrq = latency;
                } else if (pendingIrq > 0) {
                    pendingIrq--;
                }
            }
        }
    }
    printf("  %ld steps checked\n", steps);
    CHECK(bad == 0, "%ld steps with the wrong count or a stale sample", bad);
    CHECK(badHalf == 0, "%ld handler runs picked the wrong half", badHalf);
}

static void TestCycles(void)
{
    printf("cycle statistics per block\n");
    audio_cycles_t c;
    AudioCycles_Reset(&c);
    CHECK(AudioCycles_Word(&c) == 0, "not zero after a reset");

    /* Blocks of 48: the average comes after 86 blocks (4128 samples), over those 86 */
    for (int i = 0; i < 85; i++) AudioCycles_Count(&c, 1000, 48);
    CHECK((AudioCycles_Word(&c) >> AUDIO_CYCLES_AVG_OFFS) == 0, "average before enough samples");
    AudioCycles_Count(&c, 1000 + 86 * 10, 48);
    uint32_t w = AudioCycles_Word(&c);
    CHECK((w >> AUDIO_CYCLES_AVG_OFFS) == 1010, "average %u, expected 1010", w >> AUDIO_CYCLES_AVG_OFFS);
    CHECK((w & 0xFFFF) == 1860, "max %u, expected 1860", w & 0xFFFF);

    /* The next average is over the next blocks only; the maximum stays */
    for (int i = 0; i < 86; i++) AudioCycles_Count(&c, 500, 48);
    w = AudioCycles_Word(&c);
    CHECK((w >> AUDIO_CYCLES_AVG_OFFS) == 500 && (w & 0xFFFF) == 1860, "second average 0x%08X", w);

    /* The maximum sticks at 0xFFFF, and so does an overloaded average */
    AudioCycles_Count(&c, 1000000, 48);
    CHECK((AudioCycles_Word(&c) & 0xFFFF) == 0xFFFF, "max should stick at 0xFFFF");
    AudioCycles_Reset(&c);
    for (int i = 0; i < 4096; i++) AudioCycles_Count(&c, 200000, 1);
    CHECK(AudioCycles_Word(&c) == 0xFFFFFFFF, "overload 0x%08X", AudioCycles_Word(&c));

    /* Blocks of 22 (22050 Hz): 187 blocks make 4114 samples */
    AudioCycles_Reset(&c);
    for (int i = 0; i < 186; i++) AudioCycles_Count(&c, 300, 22);
    CHECK((AudioCycles_Word(&c) >> AUDIO_CYCLES_AVG_OFFS) == 0, "22: average too early");
    AudioCycles_Count(&c, 300, 22);
    CHECK((AudioCycles_Word(&c) >> AUDIO_CYCLES_AVG_OFFS) == 300, "22: average %u", AudioCycles_Word(&c) >> 16);
}

int main(void)
{
    TestLen();
    TestConverters();
    TestVolume();
    TestLoud();
    TestDmaHalves();
    TestCycles();
    printf(failures ? "\n%d FAILED\n" : "\nall tests passed\n", failures);
    return failures ? 1 : 0;
}
