/*
 * Host unit tests for the reset diagnostics (stm32/aioc-fw/Src/diag.c): what the boot code
 * makes of the record a reset left behind, the crash record, and the registers it publishes.
 * Build and run: make -C bench test
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "diag.h"
#include "settings.h"

/* The firmware defines these in main.c */
diag_record_t diagRecord;
diag_snapshot_t diagBoot;

static int failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define PORRSTF     0x08000000UL
#define PINRSTF     0x04000000UL
#define SFTRSTF     0x10000000UL
#define IWDGRSTF    0x20000000UL

#define RAM_START   0x20000000UL
#define RAM_END     0x20003F80UL

static uint32_t regs[DIAG_REG_COUNT];

static void Publish(void)
{
    memset(regs, 0xEE, sizeof(regs));
    Diag_Publish(regs, &diagBoot);
}

static void TestRegisterMap(void)
{
    printf("register map matches settings.h\n");
    CHECK(SETTINGS_REG_INFO_DIAG_COUNT == DIAG_REG_COUNT, "count");
    CHECK(SETTINGS_REG_INFO_DIAG + DIAG_REG_RESET == SETTINGS_REG_INFO_DIAGRESET, "reset");
    CHECK(SETTINGS_REG_INFO_DIAG + DIAG_REG_FAULT == SETTINGS_REG_INFO_DIAGFAULT, "fault");
    CHECK(SETTINGS_REG_INFO_DIAG + DIAG_REG_PC == SETTINGS_REG_INFO_DIAGPC, "pc");
    CHECK(SETTINGS_REG_INFO_DIAG + DIAG_REG_EXCRETURN == SETTINGS_REG_INFO_DIAGEXCRET, "excret");
    CHECK(SETTINGS_REG_INFO_DIAG + DIAG_REG_UPTIME == SETTINGS_REG_INFO_DIAGUPTIME, "uptime");
    CHECK(SETTINGS_REG_INFO_DIAG + DIAG_REG_AGE1 == SETTINGS_REG_INFO_DIAGAGE1, "age1");
    CHECK(SETTINGS_REG_INFO_DIAG + DIAG_REG_STACK == SETTINGS_REG_INFO_DIAGSTACK, "stack");
    CHECK(SETTINGS_REG_INFO_DIAGSTACK + 1 == SETTINGS_REG_INFO_DIAGLOOPS, "loops");
    CHECK(SETTINGS_REG_INFO_DIAG_MARKER == DIAG_MAGIC, "marker");
    CHECK(SETTINGS_REG_INFO_DIAG >= SETTINGS_REGMAP_READONLYADDR, "must be read only");
    CHECK(SETTINGS_REG_INFO_DIAGLOOPS < SETTINGS_REGMAP_SIZE, "fits");
}

static void TestPowerOn(void)
{
    printf("power-on: garbage is ignored, counts start at 0\n");
    memset(&diagRecord, 0x5A, sizeof(diagRecord));
    diagRecord.magic = DIAG_MAGIC;              /* even with the magic intact by chance */
    diagRecord.magic2 = DIAG_MAGIC;
    Diag_Boot(&diagRecord, &diagBoot, PORRSTF | PINRSTF);
    Publish();
    CHECK(!diagBoot.valid, "a power-on must not count as valid");
    CHECK(regs[DIAG_REG_MARKER] == DIAG_MAGIC, "marker");
    CHECK(regs[DIAG_REG_RESET] == (PORRSTF | PINRSTF), "reset word 0x%08X", regs[DIAG_REG_RESET]);
    CHECK(regs[DIAG_REG_FAULT] == 0 && regs[DIAG_REG_PC] == 0 && regs[DIAG_REG_UPTIME] == 0, "no fault, no uptime");
    CHECK(regs[DIAG_REG_AGE0] == 0xFFFFFFFFUL && regs[DIAG_REG_AGE1] == 0xFFFFFFFFUL, "ages unknown");
    CHECK((regs[DIAG_REG_STACK] & 0xFFFF) == DIAG_STACK_UNKNOWN, "stack unknown");
    CHECK(diagRecord.magic == DIAG_MAGIC && diagRecord.magic2 == DIAG_MAGIC && diagRecord.resets == 0
          && diagRecord.faults == 0 && diagRecord.tick == 0 && diagRecord.faultValid == 0, "fresh record");

    /* No power-on flag but a broken magic (as after the ST bootloader used that RAM) */
    memset(&diagRecord, 0x5A, sizeof(diagRecord));
    diagRecord.magic = DIAG_MAGIC;
    Diag_Boot(&diagRecord, &diagBoot, PINRSTF);
    CHECK(!diagBoot.valid && diagRecord.resets == 0, "a broken second magic must not count as valid");
}

/* A run: ticks advance, the main loop and interrupts leave breadcrumbs */
static void Run(uint32_t ms, uint32_t mainStopsAt, uint32_t interruptsStopAt)
{
    for (uint32_t t = 1; t <= ms; t++) {
        diagRecord.tick = t;
        if (t <= interruptsStopAt) {
            DIAG_CRUMB(usbTick);
            DIAG_CRUMB(adcTick);
            DIAG_CRUMB(dacTick);
        }
        if (t <= mainStopsAt) {
            DIAG_CRUMB(mainTick);
        }
    }
}

static void TestStarvation(void)
{
    printf("watchdog reset with the main loop starved: ages tell\n");
    memset(&diagRecord, 0, sizeof(diagRecord));
    Diag_Boot(&diagRecord, &diagBoot, PORRSTF);
    Run(60000, 60000 - 150, 60000);             /* main loop stops 150 ms before the reset */
    Diag_Boot(&diagRecord, &diagBoot, IWDGRSTF | PINRSTF);
    diagBoot.stackUnused = 2400;
    diagBoot.stackSize = 5120;
    Publish();
    CHECK(diagBoot.valid, "valid after a watchdog reset");
    CHECK(regs[DIAG_REG_RESET] == (1 | DIAG_RESET_VALID_MASK | IWDGRSTF | PINRSTF), "reset word 0x%08X", regs[DIAG_REG_RESET]);
    CHECK(regs[DIAG_REG_UPTIME] == 60000, "uptime %u", regs[DIAG_REG_UPTIME]);
    CHECK(regs[DIAG_REG_AGE0] == 150, "main 150 ms, usb 0: 0x%08X", regs[DIAG_REG_AGE0]);
    CHECK(regs[DIAG_REG_AGE1] == 0, "adc and dac 0: 0x%08X", regs[DIAG_REG_AGE1]);
    CHECK(regs[DIAG_REG_STACK] == (2400 | (5120UL << 16)), "stack 0x%08X", regs[DIAG_REG_STACK]);
    CHECK(diagRecord.tick == 0 && diagRecord.mainTick == 0 && diagRecord.resets == 1, "record cleared, count kept");

    /* Everything stops at once (tick too): the ages are all small */
    Run(5000, 5000, 5000);
    Diag_Boot(&diagRecord, &diagBoot, IWDGRSTF);
    Publish();
    CHECK((regs[DIAG_REG_RESET] & DIAG_RESET_COUNT_MASK) == 2, "second reset counted");
    CHECK(regs[DIAG_REG_AGE0] == 0 && regs[DIAG_REG_AGE1] == 0, "all stopped together");

    /* Not recording or playing: those interrupts never ran since boot */
    diagRecord.tick = 70000;
    diagRecord.mainTick = 69999;
    Diag_Boot(&diagRecord, &diagBoot, SFTRSTF);
    Publish();
    CHECK(regs[DIAG_REG_AGE0] == (1 | (0xFFFFUL << 16)), "usb never: 0x%08X", regs[DIAG_REG_AGE0]);
    CHECK(regs[DIAG_REG_AGE1] == 0xFFFFFFFFUL, "adc and dac never: 0x%08X", regs[DIAG_REG_AGE1]);
    CHECK(regs[DIAG_REG_FAULT] == 0, "no fault");

    /* Long ages saturate */
    diagRecord.tick = 200000;
    diagRecord.mainTick = 1;
    Diag_Boot(&diagRecord, &diagBoot, IWDGRSTF);
    Publish();
    CHECK((regs[DIAG_REG_AGE0] & 0xFFFF) == 0xFFFF, "saturates");
}

static void TestFault(void)
{
    printf("crash record\n");
    uint32_t stackRam[16];
    memset(&diagRecord, 0, sizeof(diagRecord));
    Diag_Boot(&diagRecord, &diagBoot, PORRSTF);
    Run(1234, 1234, 1234);

    /* The frame: r0 r1 r2 r3 r12 lr pc xpsr. The test's array is not in the AIOC's RAM range,
     * so pass its own address range as RAM */
    uint32_t frame[8] = { 1, 2, 3, 4, 12, 0x08001235, 0x08004568, 0x21000000 };
    memcpy(stackRam, frame, sizeof(frame));
    uintptr_t lo = (uintptr_t) stackRam, hi = lo + sizeof(stackRam);
    Diag_RecordFault(&diagRecord, stackRam, 0xFFFFFFF9UL, 3, 0x00008200UL, 0x40000000UL, 0xE000EDF8UL, 0x40021000UL, lo, hi);
    Diag_Boot(&diagRecord, &diagBoot, IWDGRSTF | PINRSTF);
    Publish();
    CHECK(regs[DIAG_REG_RESET] & DIAG_RESET_FAULT_MASK, "fault flag");
    CHECK(regs[DIAG_REG_FAULT] == (3 | (1UL << 16)), "fault word 0x%08X", regs[DIAG_REG_FAULT]);
    CHECK(regs[DIAG_REG_PC] == 0x08004568 && regs[DIAG_REG_LR] == 0x08001235 && regs[DIAG_REG_XPSR] == 0x21000000,
          "stacked registers 0x%08X 0x%08X 0x%08X", regs[DIAG_REG_PC], regs[DIAG_REG_LR], regs[DIAG_REG_XPSR]);
    CHECK(regs[DIAG_REG_CFSR] == 0x8200 && regs[DIAG_REG_HFSR] == 0x40000000 && regs[DIAG_REG_BFAR] == 0x40021000
          && regs[DIAG_REG_MMFAR] == 0xE000EDF8 && regs[DIAG_REG_EXCRETURN] == 0xFFFFFFF9, "status registers");
    CHECK(regs[DIAG_REG_UPTIME] == 1234, "uptime");

    /* The record is cleared at boot; the next reset reports no fault, but the count stays */
    Diag_Boot(&diagRecord, &diagBoot, SFTRSTF);
    Publish();
    CHECK(!(regs[DIAG_REG_RESET] & DIAG_RESET_FAULT_MASK) && regs[DIAG_REG_PC] == 0, "fault reported once only");
    CHECK(regs[DIAG_REG_FAULT] == (1UL << 16), "fault count kept: 0x%08X", regs[DIAG_REG_FAULT]);

    /* A frame outside RAM (a broken stack pointer) is not read */
    Diag_RecordFault(&diagRecord, (const uint32_t *) (uintptr_t) 0x10, 0xFFFFFFF9UL, 3, 0, 0x40000000UL, 0, 0,
                     RAM_START, RAM_END);
    Diag_Boot(&diagRecord, &diagBoot, IWDGRSTF);
    Publish();
    CHECK((regs[DIAG_REG_RESET] & DIAG_RESET_FAULT_MASK) && regs[DIAG_REG_PC] == 0 && regs[DIAG_REG_HFSR] == 0x40000000,
          "fault with a bad stack pointer");
    CHECK((regs[DIAG_REG_FAULT] >> 16) == 2, "second fault counted");

    /* An unexpected interrupt: IRQ 7 is exception 23 */
    Diag_RecordFault(&diagRecord, stackRam, 0xFFFFFFE9UL, 23, 0, 0, 0, 0, lo, hi);
    Diag_Boot(&diagRecord, &diagBoot, IWDGRSTF);
    Publish();
    CHECK((regs[DIAG_REG_FAULT] & DIAG_FAULT_EXC_MASK) == 23, "exception number");
}

static void TestReboot(void)
{
    printf("host-requested reboot\n");
    memset(&diagRecord, 0, sizeof(diagRecord));
    Diag_Boot(&diagRecord, &diagBoot, PORRSTF);
    diagRecord.reboot = DIAG_MAGIC_REBOOT;
    Diag_Boot(&diagRecord, &diagBoot, IWDGRSTF);
    Publish();
    CHECK(regs[DIAG_REG_RESET] & DIAG_RESET_REBOOT_MASK, "reboot flag");
    Diag_Boot(&diagRecord, &diagBoot, IWDGRSTF);
    Publish();
    CHECK(!(regs[DIAG_REG_RESET] & DIAG_RESET_REBOOT_MASK), "reboot flag cleared");

    /* Many resets: the count sticks at 0xFFFF in the register */
    diagRecord.resets = 0x12345;
    Diag_Boot(&diagRecord, &diagBoot, IWDGRSTF);
    Publish();
    CHECK((regs[DIAG_REG_RESET] & DIAG_RESET_COUNT_MASK) == 0xFFFF, "count saturates");
}

static void TestStack(void)
{
    printf("stack high-water mark\n");
    uint32_t stack[64];
    Diag_StackPaint(stack, &stack[60]);
    CHECK(Diag_StackUnused(stack, &stack[60]) == 240, "fresh paint: %u", Diag_StackUnused(stack, &stack[60]));
    stack[40] = 0;                              /* the stack reached down to word 40 */
    CHECK(Diag_StackUnused(stack, &stack[60]) == 160, "used to 40: %u", Diag_StackUnused(stack, &stack[60]));
    stack[0] = 1;                               /* it reached the bottom */
    CHECK(Diag_StackUnused(stack, &stack[60]) == 0, "reached the bottom");
}

int main(void)
{
    TestRegisterMap();
    TestPowerOn();
    TestStarvation();
    TestFault();
    TestReboot();
    TestStack();
    printf(failures ? "\n%d FAILED\n" : "\nall tests passed\n", failures);
    return failures ? 1 : 0;
}
