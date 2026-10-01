#include "stm32f3xx_hal.h"
#include "aioc.h"
#include "settings.h"
#include "led.h"
#include "usb.h"
#include "fox_hunt.h"
#include "diag.h"
#include <assert.h>
#include <io.h>
#include <stdio.h>

// from ST application note AN2606
// Table 171: Bootloader device-dependent parameters
#if defined(STM32F302xB) || defined(STM32F302xC) || \
    defined(STM32F303xB) || defined(STM32F303xC) || \
    defined(STM32F373xC)
#define SYSTEM_MEMORY_BASE 0x1FFFD800
#else
#warning Live DFU reboot not supported on this MCU
#endif

#define USB_RESET_DELAY     100 /* ms */

/* Reset diagnostics (diag.h). The record lives in .noinit at the top of RAM, which the
 * startup code neither copies nor zeroes, so it survives every reset but a power-on */
diag_record_t diagRecord __attribute__((section(".noinit")));
diag_snapshot_t diagBoot;
volatile uint32_t mainLoopPasses;

_Static_assert(RCC_CSR_PORRSTF == 0x08000000UL, "diag.c assumes this PORRSTF");
_Static_assert(SETTINGS_REG_INFO_DIAG_COUNT == DIAG_REG_COUNT, "diag register count differs");
_Static_assert(SETTINGS_REG_INFO_DIAGAGE1 == SETTINGS_REG_INFO_DIAG + DIAG_REG_AGE1, "diag register map differs");
_Static_assert(SETTINGS_REG_INFO_DIAG_MARKER == DIAG_MAGIC, "diag marker differs");

static void SystemClock_Config(void)
{
    HAL_StatusTypeDef status;

    /* Enable external oscillator and configure PLL: 8 MHz (HSE) / 1 * 9 = 72 MHz */
    RCC_OscInitTypeDef OscConfig = {
        .OscillatorType = RCC_OSCILLATORTYPE_HSE,
        .HSEState = RCC_HSE_ON,
        .HSEPredivValue = RCC_HSE_PREDIV_DIV1,
        .PLL = {
            .PLLState = RCC_PLL_ON,
            .PLLSource = RCC_CFGR_PLLSRC_HSE_PREDIV,
            .PLLMUL = RCC_PLL_MUL9
        }
    };

    status = HAL_RCC_OscConfig(&OscConfig);
    assert(status == HAL_OK);

    /* Set correct peripheral clocks. 72 MHz (PLL) / 1.5 = 48 MHz */
    RCC_PeriphCLKInitTypeDef PeriphClk = {
        .PeriphClockSelection = RCC_PERIPHCLK_USB,
        .USBClockSelection = RCC_USBCLKSOURCE_PLL_DIV1_5
    };

    status = HAL_RCCEx_PeriphCLKConfig(&PeriphClk);
    assert(status == HAL_OK);

    /* Set up divider for maximum speeds and switch clock */
    RCC_ClkInitTypeDef ClkConfig = {
        .ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2,
        .SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK,
        .AHBCLKDivider = RCC_SYSCLK_DIV1,
        .APB1CLKDivider = RCC_HCLK_DIV2,
        .APB2CLKDivider = RCC_HCLK_DIV1
    };

   status = HAL_RCC_ClockConfig(&ClkConfig, FLASH_LATENCY_2);
   assert(status == HAL_OK);

    NVIC_SetPriority(SysTick_IRQn, AIOC_IRQ_PRIO_SYSTICK);

    /* Enable MCO Pin to PLL/2 output */
    __HAL_RCC_GPIOA_CLK_ENABLE();

    GPIO_InitTypeDef GpioInit = {
        .Pin = GPIO_PIN_8,
        .Mode = GPIO_MODE_AF_PP,
        .Pull = GPIO_NOPULL,
        .Speed = GPIO_SPEED_FREQ_HIGH,
        .Alternate = GPIO_AF0_MCO
    };

    HAL_GPIO_Init(GPIOA, &GpioInit);
    HAL_RCC_MCOConfig(RCC_MCO1, RCC_MCO1SOURCE_PLLCLK_DIV2, RCC_MCODIV_1);
}

static void SystemReset(void) {
    uint32_t resetFlags = RCC->CSR;

    /* What the last run left, before the flags are cleared (published to the settings
     * registers by Settings_Init): the record, and how much of the stack it never used. Then
     * paint the stack again, all but the top, which this boot is using now */
    {
        extern uint32_t _ebss;      /* the stack runs from here ... */
        extern uint32_t _estack;    /* ... up to here (linker script) */
        uint32_t *bottom = &_ebss;
        uint32_t *top = &_estack;
        uint32_t *inUse = (uint32_t *) (__get_MSP() & ~3UL) - 32;

        Diag_Boot(&diagRecord, &diagBoot, resetFlags);
        diagBoot.stackUnused = Diag_StackUnused(bottom, inUse);
        diagBoot.stackSize = (uint32_t) (top - bottom) * sizeof(uint32_t);
        Diag_StackPaint(bottom, inUse);
    }

    /* Clear reset flags */
    RCC->CSR |= RCC_CSR_RMVF;

    /* Reset USB if necessary */
    if (!(resetFlags & RCC_CSR_PORRSTF)) {
        /* Since the USB Pullup is hardwired to the supply voltage,
         * the host (re-)enumerates our USB device only during Power-On-Reset.
         * For all other reset causes, do a manual USB reset. */
        USB_Reset();
#if 1
        /* Use SysTick to delay before continuing */
        SysTick->LOAD  = ((uint32_t) USB_RESET_DELAY * (HAL_RCC_GetHCLKFreq() / 1000)) - 1;
        SysTick->CTRL  = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_ENABLE_Msk;

        while (! (SysTick->CTRL & SysTick_CTRL_COUNTFLAG_Msk) )
            /* Wait for timer expiration */;

        SysTick->CTRL  = 0x00000000; /* Reset SysTick */
#endif
    }

    if (resetFlags & RCC_CSR_WWDGRSTF) {
#if defined(SYSTEM_MEMORY_BASE)
        /* Reset cause was watchdog, which is used for rebooting into the bootloader.
           Set stack pointer to *SYSTEM_MEMORY_BASE
           and jump to *(SYSTEM_MEMORY_BASE + 4)
           https://stackoverflow.com/a/42031657 */
        asm volatile (
            "  msr     msp, %[sp]      \n"
            "  bx      %[pc]           \n"

            :: [sp] "r" (*( (uint32_t*)(SYSTEM_MEMORY_BASE)     )),
               [pc] "r" (*( (uint32_t*)(SYSTEM_MEMORY_BASE + 4) ))
        );
#else
    while(1)
        ;
#endif
    }

    /* Initialize HAL */
    HAL_Init();

    /* Enable Clock to SYSCFG */
    __HAL_RCC_SYSCFG_CLK_ENABLE();

    /* Enable SWO debug output */
    __HAL_RCC_GPIOB_CLK_ENABLE();
    GPIO_InitTypeDef GpioSWOInit = {
        .Pin = GPIO_PIN_3,
        .Mode = GPIO_MODE_AF_PP,
        .Pull = GPIO_NOPULL,
        .Speed = GPIO_SPEED_FREQ_LOW,
        .Alternate = GPIO_AF0_TRACE
    };
    HAL_GPIO_Init(GPIOB, &GpioSWOInit);
}

int _write(int file, char *ptr, int len)
{
	for (uint32_t i=0; i<len; i++) {
		ITM_SendChar(*ptr++);
	}

	return len;
}

void _close(void)
{
}

void _lseek(void)
{
}

void _read(void)
{
}

void _fstat(void)
{
}

void _getpid(void)
{
}

void _isatty(void)
{
}

void _kill(void)
{
}

int main(void)
{
    SystemReset();
    SystemClock_Config();

    Settings_Init();

    LED_Init();
    LED_MODE(0, LED_MODE_SLOWPULSE2X);
    LED_MODE(1, LED_MODE_SLOWPULSE2X);

    IO_Init();

    USB_Init();

    FoxHunt_Init();

    /* Enable indepedent watchdog to reset on lockup*/
    IWDG_HandleTypeDef IWDGHandle = {
        .Instance = IWDG,
        .Init = {
            .Prescaler = IWDG_PRESCALER_8,
            .Reload = 0x02FF,
            .Window = 0x0FFF
        }
    };
    HAL_IWDG_Init(&IWDGHandle);

    uint32_t loopsSecond = 0;

    while (1) {
        USB_Task();
        USB_AudioTask();
        IO_Task();
        mainLoopPasses++;

        static uint32_t lastTick = 0;
        uint32_t nowTick = HAL_GetTick();

        if ((nowTick - lastTick) >= 1000) {
            lastTick = nowTick;

            /* 1 second timebase */
            FoxHunt_Tick();

            /* Main-loop passes in the last second, for the reset diagnostics */
            settingsRegMap[SETTINGS_REG_INFO_DIAGLOOPS] = mainLoopPasses - loopsSecond;
            loopsSecond = mainLoopPasses;


            usb_audio_fbstats_t fb;
            USB_AudioGetSpeakerFeedbackStats(&fb);

            usb_audio_bufstats_t buf;
            USB_AudioGetSpeakerBufferStats(&buf);

#if 0
            printf("buf: (%d/%d/%d) fb: (%06lX/%06lX/%06lX)\n",
                    buf.bufLevelMin, buf.bufLevelMax, buf.bufLevelAvg,
                    fb.feedbackMin, fb.feedbackMax, fb.feedbackAvg);
#endif
        }

        HAL_IWDG_Refresh(&IWDGHandle);
        DIAG_CRUMB(mainTick);
    }

  return 0;
}

void NMI_Handler(void) {
}

/* Fault handlers: record the fault for the reset diagnostics, then spin as before, so the
 * independent watchdog resets the AIOC about 150 ms later (the reset still shows as a
 * watchdog reset, as on firmware without the record, and a debugger can still attach to the
 * stopped state). Each entry picks the stack the exception frame went to (MSP or PSP, from
 * EXC_RETURN in LR) and passes it on. The handler for unexpected interrupts
 * (Default_Handler in the startup code) comes here too. */
void Diag_FaultEntry(uint32_t *frame, uint32_t excReturn) __attribute__((noreturn, used));
void Diag_FaultEntry(uint32_t *frame, uint32_t excReturn)
{
    extern uint32_t _sdata;     /* start of RAM (linker script) */
    extern uint32_t _estack;    /* top of the stack, just below .noinit */

    Diag_RecordFault(&diagRecord, frame, excReturn, __get_IPSR(), SCB->CFSR, SCB->HFSR, SCB->MMFAR, SCB->BFAR,
                     (uintptr_t) &_sdata, (uintptr_t) &_estack);
    __DSB();

    while (1) {
    }
}

#define FAULT_ENTRY(name) \
    __attribute__((naked)) void name(void) \
    { \
        __asm volatile ( \
            "tst   lr, #4            \n" \
            "ite   eq                \n" \
            "mrseq r0, msp           \n" \
            "mrsne r0, psp           \n" \
            "mov   r1, lr            \n" \
            "b     Diag_FaultEntry   \n" \
        ); \
    }

FAULT_ENTRY(HardFault_Handler)
FAULT_ENTRY(MemManage_Handler)
FAULT_ENTRY(BusFault_Handler)
FAULT_ENTRY(UsageFault_Handler)

void SVC_Handler(void) {
}

void DebugMon_Handler(void) {
}

void PendSV_Handler(void) {
}

void SysTick_Handler(void) {
    HAL_IncTick();
    diagRecord.tick = HAL_GetTick();
}

