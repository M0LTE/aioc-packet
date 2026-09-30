/*
 * Host unit tests for loading a stored settings page (stm32/aioc-fw/Src/settings_page.c):
 * which EQ registers a page gives you, depending on which firmware stored it.
 * Build and run: make -C bench test
 *
 * Compiled twice, as the firmware is: with the default EQ profile (k5-red) and with
 * TXEQ_DEFAULT_OFF. The firmware file is compiled unchanged.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "settings_page.h"

static int failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define EQ0   SETTINGS_REG_TXEQ_COEF0
#define EQN   (SETTINGS_REG_TXEQ_COEF_COUNT + 1)   /* coefficients and the control word */
#define CTRL  SETTINGS_REG_TXEQ_CTRL
#define MARK  SETTINGS_REG_INFO_TXEQPAGE

static const uint32_t K5_RED[EQN] = {
    0x1FC74BD8, 0xC0E42580, 0x1F551D69, 0xC0E42580, 0x1F1C6941,
    0x12B8B2CD, 0xF090A649, 0x04B022DF, 0xDF2C6FBF, 0x11C94E12,
    0x2029FDF6, 0xC3FC2B77, 0x1C737807, 0xC3FC2B77, 0x1C9D75FC,
    0xBB801303,
};
static const uint32_t CUSTOM[EQN] = {
    0x20000000, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 0,
    0x00002A01,
};
static const uint32_t ZERO[EQN];

/* What this build should load for the EQ registers when a page predates the equaliser */
#ifndef TXEQ_DEFAULT_OFF
static const uint32_t *const DEFAULTS = K5_RED;
#define BUILD_NAME "default build (k5-red)"
#else
static const uint32_t *const DEFAULTS = ZERO;
#define BUILD_NAME "TXEQ_DEFAULT_OFF build"
#endif

/* A page as stock v1.4.x stores it: its whole RAM map, with some settings changed from the
 * defaults, live debug values in the read-only area, and zero at 0xB0 to 0xBF and 0xC9 */
static void StockPage(uint32_t *page)
{
    memset(page, 0, SETTINGS_REGMAP_SIZE * sizeof(uint32_t));
    page[SETTINGS_REG_MAGIC] = SETTINGS_REG_MAGIC_DEFAULT;
    page[SETTINGS_REG_USBID] = SETTINGS_REG_USBID_DEFAULT;
    page[SETTINGS_REG_AIOC_IOMUX0] = SETTINGS_REG_AIOC_IOMUX0_OUT1SRC_CM108GPIO3_MASK;   /* user's PTT */
    page[SETTINGS_REG_AIOC_IOMUX1] = SETTINGS_REG_AIOC_IOMUX1_DEFAULT;
    page[SETTINGS_REG_VPTT_LVLCTRL] = 0x40;
    page[SETTINGS_REG_FOXHUNT_MSG0] = 0x3059304D;
    page[SETTINGS_REG_INFO_AIOC0] = 0x00010000;
    page[SETTINGS_REG_INFO_AUDIO2] = 48000;
    page[0xFF] = 0x12345678;
}

static void SetEq(uint32_t *regs, const uint32_t *eq)
{
    memcpy(&regs[EQ0], eq, EQN * sizeof(uint32_t));
}

static int EqIs(const uint32_t *regs, const uint32_t *eq)
{
    return memcmp(&regs[EQ0], eq, EQN * sizeof(uint32_t)) == 0;
}

/* Every register outside the EQ block and the marker is the page's, word for word */
static int RestIsPage(const uint32_t *regs, const uint32_t *page)
{
    for (int a = 0; a < SETTINGS_REGMAP_SIZE; a++) {
        if ((a >= EQ0 && a <= CTRL) || a == MARK) continue;
        if (regs[a] != page[a]) return 0;
    }
    return 1;
}

static void Load(uint32_t *regs, const uint32_t *page)
{
    for (int a = 0; a < SETTINGS_REGMAP_SIZE; a++) regs[a] = 0xDEADBEEF;   /* whatever was in RAM */
    SettingsPage_Load(regs, page);
}

static void TestStockPage(void)
{
    uint32_t page[SETTINGS_REGMAP_SIZE], regs[SETTINGS_REGMAP_SIZE];
    StockPage(page);
    CHECK(!SettingsPage_EqFromPage(page), "a stock page must not count as having EQ settings");
    Load(regs, page);
    CHECK(EqIs(regs, DEFAULTS), "stock page: the EQ registers must be the build's defaults");
    CHECK(RestIsPage(regs, page), "stock page: every other setting must come from the page");
    CHECK(regs[MARK] == SETTINGS_REG_INFO_TXEQPAGE_MARKER, "the marker must be set after a recall");
}

static void TestForkPageEqOff(void)
{
    /* Stored by this firmware after "aioc_eq.py off --store": EQ block all zero, marker set */
    uint32_t page[SETTINGS_REGMAP_SIZE], regs[SETTINGS_REGMAP_SIZE];
    StockPage(page);
    page[MARK] = SETTINGS_REG_INFO_TXEQPAGE_MARKER;
    CHECK(SettingsPage_EqFromPage(page), "a page with the marker must count as having EQ settings");
    Load(regs, page);
    CHECK(EqIs(regs, ZERO), "a stored 'off' must stay off");
    CHECK(RestIsPage(regs, page), "fork page: every other setting must come from the page");
}

static void TestForkPageEqOn(void)
{
    uint32_t page[SETTINGS_REGMAP_SIZE], regs[SETTINGS_REGMAP_SIZE];
    StockPage(page);
    page[MARK] = SETTINGS_REG_INFO_TXEQPAGE_MARKER;
    SetEq(page, CUSTOM);
    Load(regs, page);
    CHECK(EqIs(regs, CUSTOM), "a stored custom set must load as stored");
    SetEq(page, K5_RED);
    Load(regs, page);
    CHECK(EqIs(regs, K5_RED), "a stored k5-red set must load as stored");
}

static void TestForkPageStaleCoefsOff(void)
{
    /* Control word 0 with coefficients left behind, stored by this firmware: off, as stored */
    uint32_t page[SETTINGS_REGMAP_SIZE], regs[SETTINGS_REGMAP_SIZE], eq[EQN];
    StockPage(page);
    page[MARK] = SETTINGS_REG_INFO_TXEQPAGE_MARKER;
    memcpy(eq, K5_RED, sizeof(eq));
    eq[EQN - 1] = 0;
    SetEq(page, eq);
    Load(regs, page);
    CHECK(EqIs(regs, eq), "marker page with control word 0 must load exactly as stored");
}

static void TestPacket1Pages(void)
{
    /* v1.4.1-packet.1 stored no marker. With the EQ stored on, the control word is nonzero */
    uint32_t page[SETTINGS_REGMAP_SIZE], regs[SETTINGS_REGMAP_SIZE];
    StockPage(page);
    SetEq(page, K5_RED);
    CHECK(SettingsPage_EqFromPage(page), "a nonzero control word must count as having EQ settings");
    Load(regs, page);
    CHECK(EqIs(regs, K5_RED), "packet.1 page with k5-red stored must keep it");
    SetEq(page, CUSTOM);
    Load(regs, page);
    CHECK(EqIs(regs, CUSTOM), "packet.1 page with a custom set stored must keep it");
    CHECK(RestIsPage(regs, page), "packet.1 page: every other setting must come from the page");
    CHECK(regs[MARK] == SETTINGS_REG_INFO_TXEQPAGE_MARKER, "the marker must be set after a recall");
}

static void TestOtherMarkerValue(void)
{
    /* Anything other than the exact marker is not the marker */
    uint32_t page[SETTINGS_REGMAP_SIZE], regs[SETTINGS_REGMAP_SIZE];
    StockPage(page);
    page[MARK] = SETTINGS_REG_INFO_TXEQPAGE_MARKER ^ 1;
    Load(regs, page);
    CHECK(EqIs(regs, DEFAULTS), "a wrong marker value must be treated as no marker");
}

static void TestRoundTrip(void)
{
    /* This firmware: defaults, store (the whole RAM map), recall gives the same map. Then the
     * user switches the EQ off and stores: it stays off over the next power-up */
    uint32_t ram[SETTINGS_REGMAP_SIZE], page[SETTINGS_REGMAP_SIZE], regs[SETTINGS_REGMAP_SIZE];
    StockPage(ram);
    SettingsPage_EqDefaults(ram);
    ram[MARK] = SETTINGS_REG_INFO_TXEQPAGE_DEFAULT;
    CHECK(EqIs(ram, DEFAULTS), "SettingsPage_EqDefaults must write the build's defaults");

    memcpy(page, ram, sizeof(page));
    Load(regs, page);
    CHECK(memcmp(regs, ram, sizeof(regs)) == 0, "store then recall must give back the same registers");

    SetEq(ram, ZERO);
    memcpy(page, ram, sizeof(page));
    Load(regs, page);
    CHECK(memcmp(regs, ram, sizeof(regs)) == 0, "stored off, recalled off");

    /* And again from the recalled map, as a second power-cycle would */
    memcpy(page, regs, sizeof(page));
    Load(regs, page);
    CHECK(EqIs(regs, ZERO), "still off after a second store and recall");
}

int main(void)
{
    printf("settings page tests, %s\n", BUILD_NAME);
    TestStockPage();
    TestForkPageEqOff();
    TestForkPageEqOn();
    TestForkPageStaleCoefsOff();
    TestPacket1Pages();
    TestOtherMarkerValue();
    TestRoundTrip();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
