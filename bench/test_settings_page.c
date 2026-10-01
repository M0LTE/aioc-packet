/*
 * Host unit tests for loading a stored settings page (stm32/aioc-fw/Src/settings_page.c):
 * which TX and RX EQ registers a page gives you, depending on which firmware stored it.
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
#define RXCTL SETTINGS_REG_RXEQ_CTRL
#define RXMRK SETTINGS_REG_INFO_RXEQPAGE

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
#define RX_DEFAULT 1u
#define BUILD_NAME "default build (k5-red, RX EQ on)"
#else
static const uint32_t *const DEFAULTS = ZERO;
#define RX_DEFAULT 0u
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

/* Every register outside the EQ registers and markers is the page's, word for word */
static int RestIsPage(const uint32_t *regs, const uint32_t *page)
{
    for (int a = 0; a < SETTINGS_REGMAP_SIZE; a++) {
        if ((a >= EQ0 && a <= CTRL) || a == MARK) continue;
        if (a == RXCTL || a == RXMRK || a == SETTINGS_REG_INFO_RXEQ || a == SETTINGS_REG_INFO_RXEQCYC) continue;
        if (a == SETTINGS_REG_INFO_RXCYC || a == SETTINGS_REG_INFO_TXCYC) continue;
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
    CHECK(!SettingsPage_RxEqFromPage(page), "a stock page must not count as having RX EQ settings");
    CHECK(regs[RXCTL] == RX_DEFAULT, "stock page: RXEQ_CTRL 0x%08X, expected the default", regs[RXCTL]);
    CHECK(regs[RXMRK] == SETTINGS_REG_INFO_RXEQPAGE_MARKER, "the RX marker must be set after a recall");
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

static void TestPacket2Page(void)
{
    /* v1.4.1-packet.2 stored the TX marker but knew nothing of the RX equaliser: RXEQ_CTRL is
     * zero on its pages, and it gets the default. The TX registers load as stored */
    uint32_t page[SETTINGS_REGMAP_SIZE], regs[SETTINGS_REGMAP_SIZE];
    StockPage(page);
    page[MARK] = SETTINGS_REG_INFO_TXEQPAGE_MARKER;
    Load(regs, page);
    CHECK(EqIs(regs, ZERO), "packet.2 page with TX EQ off: stays off");
    CHECK(regs[RXCTL] == RX_DEFAULT, "packet.2 page: RXEQ_CTRL 0x%08X, expected the default", regs[RXCTL]);
    CHECK(RestIsPage(regs, page), "packet.2 page: every other setting must come from the page");
    SetEq(page, K5_RED);
    Load(regs, page);
    CHECK(EqIs(regs, K5_RED) && regs[RXCTL] == RX_DEFAULT, "packet.2 page with TX EQ on");
}

static void TestRxPages(void)
{
    /* Stored by this firmware: both markers. RXEQ_CTRL loads exactly as stored, off included */
    static const uint32_t values[] = { 0, 1, 7, 0xFFFFFFFF };
    uint32_t page[SETTINGS_REGMAP_SIZE], regs[SETTINGS_REGMAP_SIZE];
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        StockPage(page);
        page[MARK] = SETTINGS_REG_INFO_TXEQPAGE_MARKER;
        page[RXMRK] = SETTINGS_REG_INFO_RXEQPAGE_MARKER;
        page[RXCTL] = values[i];
        page[SETTINGS_REG_INFO_RXEQ] = 0x00030101;        /* live status when it was stored */
        page[SETTINGS_REG_INFO_RXEQCYC] = 0x00C800F0;
        page[SETTINGS_REG_INFO_RXCYC] = 0x2EE03A98;
        page[SETTINGS_REG_INFO_TXCYC] = 0x0FA01200;
        CHECK(SettingsPage_RxEqFromPage(page), "a page with the RX marker must count as having RX EQ settings");
        Load(regs, page);
        CHECK(regs[RXCTL] == values[i], "RX marker page: RXEQ_CTRL 0x%08X, stored 0x%08X", regs[RXCTL], values[i]);
        CHECK(regs[SETTINGS_REG_INFO_RXEQ] == 0 && regs[SETTINGS_REG_INFO_RXEQCYC] == 0,
              "the RX status registers must start at zero, not the page's");
        CHECK(regs[SETTINGS_REG_INFO_RXCYC] == 0 && regs[SETTINGS_REG_INFO_TXCYC] == 0,
              "the audio cycle counts must start at zero, not the page's");
        CHECK(RestIsPage(regs, page), "RX marker page: every other setting must come from the page");
    }

    /* A wrong RX marker value is no marker; the RX marker does not affect the TX rules */
    StockPage(page);
    page[RXMRK] = SETTINGS_REG_INFO_RXEQPAGE_MARKER ^ 0x100;
    page[RXCTL] = 0;
    Load(regs, page);
    CHECK(regs[RXCTL] == RX_DEFAULT, "a wrong RX marker value must be treated as no marker");
    StockPage(page);
    page[RXMRK] = SETTINGS_REG_INFO_RXEQPAGE_MARKER;
    Load(regs, page);
    CHECK(EqIs(regs, DEFAULTS), "an RX marker alone must not make the TX registers count as stored");
}

static void TestRoundTrip(void)
{
    /* This firmware: defaults, store (the whole RAM map), recall gives the same map. Then the
     * user switches the EQ off and stores: it stays off over the next power-up */
    uint32_t ram[SETTINGS_REGMAP_SIZE], page[SETTINGS_REGMAP_SIZE], regs[SETTINGS_REGMAP_SIZE];
    StockPage(ram);
    SettingsPage_EqDefaults(ram);
    ram[MARK] = SETTINGS_REG_INFO_TXEQPAGE_DEFAULT;
    ram[RXMRK] = SETTINGS_REG_INFO_RXEQPAGE_DEFAULT;
    CHECK(EqIs(ram, DEFAULTS), "SettingsPage_EqDefaults must write the build's defaults");
    CHECK(ram[RXCTL] == RX_DEFAULT, "SettingsPage_EqDefaults must write the build's RX default");

    memcpy(page, ram, sizeof(page));
    Load(regs, page);
    CHECK(memcmp(regs, ram, sizeof(regs)) == 0, "store then recall must give back the same registers");

    SetEq(ram, ZERO);
    ram[RXCTL] = 0;
    memcpy(page, ram, sizeof(page));
    Load(regs, page);
    CHECK(memcmp(regs, ram, sizeof(regs)) == 0, "stored off, recalled off");

    /* And again from the recalled map, as a second power-cycle would */
    memcpy(page, regs, sizeof(page));
    Load(regs, page);
    CHECK(EqIs(regs, ZERO), "still off after a second store and recall");
    CHECK(regs[RXCTL] == 0, "RX EQ still off after a second store and recall");

    /* RX on again, stored: on after the next power-up, whatever the build's default */
    regs[RXCTL] = 1;
    memcpy(page, regs, sizeof(page));
    Load(regs, page);
    CHECK(regs[RXCTL] == 1, "RX EQ stored on, recalled on");
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
    TestPacket2Page();
    TestRxPages();
    TestRoundTrip();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
