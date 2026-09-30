#ifndef SETTINGS_PAGE_H_
#define SETTINGS_PAGE_H_

/* Loading a stored settings page into the register map, and the TX equaliser defaults.
 * No HAL here, so bench/test_settings_page.c can compile this file for the host. */

#include <stdint.h>
#include "settings.h"

/* Nonzero if the EQ registers on a stored page (one with the AIOC magic) are the user's own:
 * the page carries SETTINGS_REG_INFO_TXEQPAGE, or its TXEQ_CTRL is nonzero. Zero for a page
 * stored by firmware without the equaliser, which holds zero there */
uint8_t SettingsPage_EqFromPage(const volatile uint32_t *page);

/* Copy a stored page into regs, from address 0 upwards, so TXEQ_CTRL is written after the
 * coefficients. The EQ registers get their defaults instead if SettingsPage_EqFromPage says
 * the page predates the equaliser. SETTINGS_REG_INFO_TXEQPAGE always ends up as the marker */
void SettingsPage_Load(volatile uint32_t *regs, const volatile uint32_t *page);

/* Write the EQ defaults: the coefficients first, then the control word that commits them */
void SettingsPage_EqDefaults(volatile uint32_t *regs);

#endif /* SETTINGS_PAGE_H_ */
