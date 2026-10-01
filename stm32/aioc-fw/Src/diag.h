#ifndef DIAG_H_
#define DIAG_H_

/*
 * Reset diagnostics (aioc-packet): why did the AIOC last reset?
 *
 * A small record in RAM that the startup code neither copies nor zeroes (section .noinit,
 * at the top of RAM, see stm32f30_flash.ld), so it survives every reset except a power-on:
 *   - a crash record, written by the fault handlers (HardFault, MemManage, BusFault,
 *     UsageFault) and by the handler for unexpected interrupts: the stacked PC, LR and xPSR,
 *     the fault status registers and which exception it was. The handlers then spin as
 *     before, so the independent watchdog resets the AIOC about 150 ms later;
 *   - breadcrumbs: the 1 ms tick, and the tick at which the main loop, the USB interrupt and
 *     the recording and playback (DMA block) interrupts last ran. After a watchdog reset they
 *     say whether the interrupts kept running while the main loop did not (starvation, or
 *     the main loop stuck) or whether everything stopped at once (a lockup at interrupt
 *     level);
 *   - a flag set when the host asked for a reboot (which also goes through the watchdog);
 *   - counts of resets and faults since power-on;
 *   - the stack's high-water mark: at boot the free stack area is painted with a pattern;
 *     the stack area is not cleared by a reset either, so at the next boot, the paint still
 *     intact at the bottom shows how close the last run came to overflowing. A stack overflow
 *     on this chip runs silently into .bss (no fault), so this is how it shows.
 *
 * At boot, Diag_Boot takes a snapshot of the record together with the reset flags from
 * RCC->CSR, then clears the record, so the next reset is reported fresh. Diag_Publish puts
 * the snapshot into the read-only settings registers SETTINGS_REG_INFO_DIAG*, where
 * tools/aioc_eq.py diag reads it.
 *
 * This file and diag.c have no hardware dependencies (the handlers that read the hardware
 * are in main.c), so the same code is compiled on the host for bench/test_diag.c.
 */

#include <stdint.h>

#define DIAG_MAGIC          0x47414944UL    /* "DIAG": the record is ours and not power-on garbage */
#define DIAG_MAGIC_REBOOT   0x544F4F42UL    /* "BOOT": the host asked for a reboot */
#define DIAG_STACK_PAINT    0xA5C3A5C3UL    /* fill pattern of the unused stack */

/* The record in .noinit. The magic is at both ends: anything that overwrote part of the top
 * of RAM (the ST bootloader, for one) most likely breaks one of them */
typedef struct {
    uint32_t magic;
    uint32_t resets;        /* resets since power-on */
    uint32_t faults;        /* faults since power-on */
    uint32_t faultValid;    /* DIAG_MAGIC: the crash record below is from the last run */
    uint32_t exception;     /* IPSR: 3 HardFault, 4 MemManage, 5 BusFault, 6 UsageFault, >= 16 an IRQ */
    uint32_t pc, lr, xpsr;  /* stacked by the exception (0 if the stack pointer was not in RAM) */
    uint32_t cfsr, hfsr, mmfar, bfar;
    uint32_t excReturn;     /* LR on exception entry */
    uint32_t reboot;        /* DIAG_MAGIC_REBOOT: the host asked for a reboot */
    uint32_t tick;          /* the 1 ms tick, written by SysTick */
    uint32_t mainTick;      /* tick at which each last ran (0: not since boot) */
    uint32_t usbTick;
    uint32_t adcTick;
    uint32_t dacTick;
    uint32_t magic2;
} diag_record_t;

/* What the last run left, taken at boot */
typedef struct {
    uint32_t csr;           /* RCC->CSR as read at boot */
    uint8_t valid;          /* the record survived (not a power-on), so the rest means something */
    uint32_t stackUnused;   /* bytes at the bottom of the stack the last run never touched */
    uint32_t stackSize;     /* bytes of stack in all */
    diag_record_t rec;
} diag_snapshot_t;

/* Registers (identical to SETTINGS_REG_INFO_DIAG*, see settings.h), in order from
 * SETTINGS_REG_INFO_DIAG */
enum {
    DIAG_REG_MARKER,        /* "DIAG" */
    DIAG_REG_RESET,         /* reset flags and counts */
    DIAG_REG_FAULT,         /* exception number and fault count */
    DIAG_REG_PC,
    DIAG_REG_LR,
    DIAG_REG_XPSR,
    DIAG_REG_CFSR,
    DIAG_REG_HFSR,
    DIAG_REG_MMFAR,
    DIAG_REG_BFAR,
    DIAG_REG_EXCRETURN,
    DIAG_REG_UPTIME,        /* ms from boot to the last tick before the reset */
    DIAG_REG_AGE0,          /* main loop and USB interrupt: ms from their last run to the last tick */
    DIAG_REG_AGE1,          /* recording and playback interrupts, likewise */
    DIAG_REG_STACK,         /* stack never used by the last run, and stack size, in bytes */
    DIAG_REG_COUNT
};

/* DIAG_REG_RESET fields */
#define DIAG_RESET_COUNT_MASK       0x0000FFFFUL    /* resets since power-on, sticks at 0xFFFF */
#define DIAG_RESET_FAULT_MASK       0x00010000UL    /* a crash record is present */
#define DIAG_RESET_REBOOT_MASK      0x00020000UL    /* the host asked for a reboot */
#define DIAG_RESET_VALID_MASK       0x00040000UL    /* the record survived: breadcrumbs and counts mean something */
#define DIAG_RESET_CSR_MASK         0xFF800000UL    /* RCC->CSR bits 23 to 31, in place */

/* DIAG_REG_FAULT fields */
#define DIAG_FAULT_EXC_MASK         0x000001FFUL
#define DIAG_FAULT_COUNT_OFFS       16

/* DIAG_REG_AGE0 and AGE1: two 16-bit ages; 0xFFFF means not since boot, or that long or longer */
#define DIAG_AGE_NEVER              0xFFFFUL

/* DIAG_REG_STACK: bits 0-15 bytes never used (0xFFFF: unknown, after a power-on), bits 16-31
 * the stack size */
#define DIAG_STACK_UNKNOWN          0xFFFFUL

/*
 * At boot, before the reset flags are cleared: take the snapshot of what the last run left
 * and start a fresh record. csr is RCC->CSR. After a power-on (PORRSTF) or with a broken
 * magic, the record is garbage: it is reset and the counts start from zero.
 */
void Diag_Boot(diag_record_t *rec, diag_snapshot_t *snap, uint32_t csr);

/*
 * From a fault handler: fill in the crash record. frame is the stacked exception frame
 * (r0, r1, r2, r3, r12, lr, pc, xpsr), read only if it lies within [ramStart, ramEnd), so a
 * fault caused by a broken stack pointer cannot fault again here.
 */
void Diag_RecordFault(diag_record_t *rec, const uint32_t *frame, uint32_t excReturn, uint32_t exception,
                      uint32_t cfsr, uint32_t hfsr, uint32_t mmfar, uint32_t bfar,
                      uintptr_t ramStart, uintptr_t ramEnd);

/* Bytes from bottom upwards that still hold DIAG_STACK_PAINT, and painting [bottom, end) */
uint32_t Diag_StackUnused(const uint32_t *bottom, const uint32_t *top);
void Diag_StackPaint(uint32_t *bottom, uint32_t *end);

/* Write the snapshot into the DIAG_REG_COUNT registers starting at regs[0] */
void Diag_Publish(volatile uint32_t *regs, const diag_snapshot_t *snap);

/* The firmware's record (in .noinit) and boot snapshot, defined in main.c */
extern diag_record_t diagRecord;
extern diag_snapshot_t diagBoot;

/* Counts main-loop passes (main.c), for DIAGLOOPS and the RX equaliser's overload guard */
extern volatile uint32_t mainLoopPasses;

/* Breadcrumb: note the current tick in a field of diagRecord. One load and one store */
#define DIAG_CRUMB(field) \
    (*(volatile uint32_t *) &diagRecord.field = *(volatile uint32_t *) &diagRecord.tick)

#endif /* DIAG_H_ */
