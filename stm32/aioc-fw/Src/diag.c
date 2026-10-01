#include "diag.h"
#include <string.h>

/* RCC->CSR power-on reset flag (checked against the CMSIS name in main.c) */
#define DIAG_CSR_PORRSTF    0x08000000UL

void Diag_Boot(diag_record_t *rec, diag_snapshot_t *snap, uint32_t csr)
{
    uint8_t valid = !(csr & DIAG_CSR_PORRSTF) && (rec->magic == DIAG_MAGIC) && (rec->magic2 == DIAG_MAGIC);

    snap->csr = csr;
    snap->valid = valid;
    if (valid) {
        memcpy(&snap->rec, rec, sizeof(snap->rec));
    } else {
        memset(&snap->rec, 0, sizeof(snap->rec));
    }

    /* A fresh record: the counts carry on (from zero after a power-on), everything else is
     * cleared, so the next reset is reported on its own */
    uint32_t resets = valid ? rec->resets : 0;
    uint32_t faults = valid ? rec->faults : 0;
    memset(rec, 0, sizeof(*rec));
    rec->magic = DIAG_MAGIC;
    rec->magic2 = DIAG_MAGIC;
    rec->faults = faults;
    rec->resets = valid ? ((resets < 0xFFFFFFFFUL) ? resets + 1 : resets) : 0;
    snap->rec.resets = rec->resets;
}

void Diag_RecordFault(diag_record_t *rec, const uint32_t *frame, uint32_t excReturn, uint32_t exception,
                      uint32_t cfsr, uint32_t hfsr, uint32_t mmfar, uint32_t bfar,
                      uintptr_t ramStart, uintptr_t ramEnd)
{
    uintptr_t sp = (uintptr_t) frame;

    if ((sp >= ramStart) && (sp <= ramEnd - 8 * sizeof(uint32_t)) && !(sp & 3)) {
        rec->lr = frame[5];
        rec->pc = frame[6];
        rec->xpsr = frame[7];
    } else {
        rec->lr = 0;
        rec->pc = 0;
        rec->xpsr = 0;
    }
    rec->exception = exception;
    rec->cfsr = cfsr;
    rec->hfsr = hfsr;
    rec->mmfar = mmfar;
    rec->bfar = bfar;
    rec->excReturn = excReturn;
    if (rec->faults < 0xFFFFFFFFUL) {
        rec->faults++;
    }
    rec->faultValid = DIAG_MAGIC;
}

uint32_t Diag_StackUnused(const uint32_t *bottom, const uint32_t *top)
{
    const uint32_t *p = bottom;
    while ((p < top) && (*p == DIAG_STACK_PAINT)) {
        p++;
    }
    return (uint32_t) (p - bottom) * sizeof(uint32_t);
}

void Diag_StackPaint(uint32_t *bottom, uint32_t *end)
{
    for (uint32_t *p = bottom; p < end; p++) {
        *p = DIAG_STACK_PAINT;
    }
}

static uint32_t Age(uint32_t now, uint32_t then)
{
    if (then == 0) {
        return DIAG_AGE_NEVER;
    }
    uint32_t age = now - then;
    return (age < DIAG_AGE_NEVER) ? age : DIAG_AGE_NEVER;
}

void Diag_Publish(volatile uint32_t *regs, const diag_snapshot_t *snap)
{
    const diag_record_t *r = &snap->rec;
    uint8_t fault = snap->valid && (r->faultValid == DIAG_MAGIC);
    uint8_t reboot = snap->valid && (r->reboot == DIAG_MAGIC_REBOOT);
    uint32_t resets = (r->resets < DIAG_RESET_COUNT_MASK) ? r->resets : DIAG_RESET_COUNT_MASK;
    uint32_t faults = (r->faults < 0xFFFFUL) ? r->faults : 0xFFFFUL;

    regs[DIAG_REG_MARKER] = DIAG_MAGIC;
    regs[DIAG_REG_RESET] = resets
                         | (fault ? DIAG_RESET_FAULT_MASK : 0)
                         | (reboot ? DIAG_RESET_REBOOT_MASK : 0)
                         | (snap->valid ? DIAG_RESET_VALID_MASK : 0)
                         | (snap->csr & DIAG_RESET_CSR_MASK);
    regs[DIAG_REG_FAULT] = (fault ? (r->exception & DIAG_FAULT_EXC_MASK) : 0) | (faults << DIAG_FAULT_COUNT_OFFS);
    regs[DIAG_REG_PC] = fault ? r->pc : 0;
    regs[DIAG_REG_LR] = fault ? r->lr : 0;
    regs[DIAG_REG_XPSR] = fault ? r->xpsr : 0;
    regs[DIAG_REG_CFSR] = fault ? r->cfsr : 0;
    regs[DIAG_REG_HFSR] = fault ? r->hfsr : 0;
    regs[DIAG_REG_MMFAR] = fault ? r->mmfar : 0;
    regs[DIAG_REG_BFAR] = fault ? r->bfar : 0;
    regs[DIAG_REG_EXCRETURN] = fault ? r->excReturn : 0;
    regs[DIAG_REG_UPTIME] = snap->valid ? r->tick : 0;
    if (snap->valid) {
        regs[DIAG_REG_AGE0] = Age(r->tick, r->mainTick) | (Age(r->tick, r->usbTick) << 16);
        regs[DIAG_REG_AGE1] = Age(r->tick, r->adcTick) | (Age(r->tick, r->dacTick) << 16);
    } else {
        regs[DIAG_REG_AGE0] = (DIAG_AGE_NEVER << 16) | DIAG_AGE_NEVER;
        regs[DIAG_REG_AGE1] = (DIAG_AGE_NEVER << 16) | DIAG_AGE_NEVER;
    }
    uint32_t unused = (snap->valid && snap->stackUnused < DIAG_STACK_UNKNOWN) ? snap->stackUnused : DIAG_STACK_UNKNOWN;
    uint32_t size = (snap->stackSize < 0xFFFFUL) ? snap->stackSize : 0xFFFFUL;
    regs[DIAG_REG_STACK] = unused | (size << 16);
}
