#include "settings_page.h"

static const uint32_t txEqCoefDefaults[SETTINGS_REG_TXEQ_COEF_COUNT] = SETTINGS_REG_TXEQ_COEF_DEFAULTS;

/* The EQ block is the coefficients followed directly by the control word */
_Static_assert(SETTINGS_REG_TXEQ_COEF0 + SETTINGS_REG_TXEQ_COEF_COUNT == SETTINGS_REG_TXEQ_CTRL,
               "TXEQ_CTRL must sit directly above the coefficients");
_Static_assert(SETTINGS_REG_INFO_TXEQPAGE >= SETTINGS_REGMAP_READONLYADDR,
               "the page marker must be read only, so the host cannot clear or forge it");

uint8_t SettingsPage_EqFromPage(const volatile uint32_t *page)
{
    return (page[SETTINGS_REG_INFO_TXEQPAGE] == SETTINGS_REG_INFO_TXEQPAGE_MARKER)
        || (page[SETTINGS_REG_TXEQ_CTRL] != 0);
}

void SettingsPage_Load(volatile uint32_t *regs, const volatile uint32_t *page)
{
    uint8_t eqFromPage = SettingsPage_EqFromPage(page);

    for (uint32_t addr = 0; addr < SETTINGS_REGMAP_SIZE; addr++) {
        uint32_t value = page[addr];

        if (!eqFromPage && (addr >= SETTINGS_REG_TXEQ_COEF0) && (addr < SETTINGS_REG_TXEQ_CTRL)) {
            value = txEqCoefDefaults[addr - SETTINGS_REG_TXEQ_COEF0];
        } else if (!eqFromPage && (addr == SETTINGS_REG_TXEQ_CTRL)) {
            value = SETTINGS_REG_TXEQ_CTRL_DEFAULT;
        } else if (addr == SETTINGS_REG_INFO_TXEQPAGE) {
            value = SETTINGS_REG_INFO_TXEQPAGE_MARKER;
        }

        /* Ascending and volatile: the audio interrupt may run between two words, and it only
         * takes a coefficient set when TXEQ_CTRL changes, which is written after them */
        regs[addr] = value;
    }
}

void SettingsPage_EqDefaults(volatile uint32_t *regs)
{
    for (uint32_t i = 0; i < SETTINGS_REG_TXEQ_COEF_COUNT; i++) {
        regs[SETTINGS_REG_TXEQ_COEF0 + i] = txEqCoefDefaults[i];
    }
    regs[SETTINGS_REG_TXEQ_CTRL] = SETTINGS_REG_TXEQ_CTRL_DEFAULT;
}
