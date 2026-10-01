#include "io.h"
#include "usb_hid.h"
#include "usb_serial.h"
#include "settings.h"

/* Input changes, from the EXTI interrupt to IO_Task: bit 0 input 1, bit 1 input 2 */
static volatile uint8_t inPending;  /* changed and not yet shown */
static volatile uint8_t inState;    /* state at the last change, 1 = active (pin low) */

void IO_IN_EXTI_ISR(void)
{
    /* Note the change only. The HID reports go out from the main loop (IO_Task): tinyusb is
     * not re-entrant, and this interrupt can cut into the main loop's USB task anywhere */
    uint32_t pr = EXTI->PR & (IO_IN_PIN_1_EXTI_PR | IO_IN_PIN_2_EXTI_PR);

    /* Clear the flags seen first, then read the pins: an edge after this read sets its flag
     * again and comes back as a new interrupt, never lost */
    EXTI->PR = pr;
    uint32_t idr = IO_IN_GPIO->IDR;
    uint8_t state = inState;
    uint8_t pending = inPending;

    if (pr & IO_IN_PIN_1_EXTI_PR) {
        state = (state & ~0x01) | ((idr & IO_IN_PIN_1) ? 0x00 : 0x01);
        pending |= 0x01;
    }

    if (pr & IO_IN_PIN_2_EXTI_PR) {
        state = (state & ~0x02) | ((idr & IO_IN_PIN_2) ? 0x00 : 0x02);
        pending |= 0x02;
    }

    inState = state;
    inPending = pending;
}

void IO_Task(void)
{
    if (!inPending) {
        return;
    }

    __disable_irq();
    uint8_t pending = inPending;
    uint8_t states = inState;
    inPending = 0;
    __enable_irq();

    if (pending & 0x01) {
        uint8_t state = states & 0x01;

        if (settingsRegMap[SETTINGS_REG_CM108_IOMUX0] & SETTINGS_REG_CM108_IOMUX0_BTN1SRC_IN1_MASK) {
            USB_HIDSendButtonState(state & 0x01 ? USB_HID_BUTTON_VOLUP : 0x00);
        }

        if (settingsRegMap[SETTINGS_REG_CM108_IOMUX1] & SETTINGS_REG_CM108_IOMUX1_BTN2SRC_IN1_MASK) {
            USB_HIDSendButtonState(state & 0x01 ? USB_HID_BUTTON_VOLDN : 0x00);
        }

        if (settingsRegMap[SETTINGS_REG_CM108_IOMUX2] & SETTINGS_REG_CM108_IOMUX2_BTN3SRC_IN1_MASK) {
            USB_HIDSendButtonState(state & 0x01 ? USB_HID_BUTTON_PLAYMUTE : 0x00);
        }

        if (settingsRegMap[SETTINGS_REG_CM108_IOMUX3] & SETTINGS_REG_CM108_IOMUX3_BTN4SRC_IN1_MASK) {
            USB_HIDSendButtonState(state & 0x01 ? USB_HID_BUTTON_RECMUTE : 0x00);
        }

        if (settingsRegMap[SETTINGS_REG_SERIAL_IOMUX0] & SETTINGS_REG_SERIAL_IOMUX0_DCDSRC_IN1_MASK) {
            USB_SerialSendLineState(state & 0x01 ? USB_SERIAL_LINESTATE_DCD : 0x00);
        }

        if (settingsRegMap[SETTINGS_REG_SERIAL_IOMUX1] & SETTINGS_REG_SERIAL_IOMUX1_DSRSRC_IN1_MASK) {
            USB_SerialSendLineState(state & 0x01 ? USB_SERIAL_LINESTATE_DSR : 0x00);
        }

        if (settingsRegMap[SETTINGS_REG_SERIAL_IOMUX2] & SETTINGS_REG_SERIAL_IOMUX2_RISRC_IN1_MASK) {
            USB_SerialSendLineState(state & 0x01 ? USB_SERIAL_LINESTATE_RI : 0x00);
        }

        if (settingsRegMap[SETTINGS_REG_SERIAL_IOMUX3] & SETTINGS_REG_SERIAL_IOMUX3_BRKSRC_IN1_MASK) {
            USB_SerialSendLineState(state & 0x01 ? USB_SERIAL_LINESTATE_BREAK : 0x00);
        }
    }

    if (pending & 0x02) {
        uint8_t state = (states >> 1) & 0x01;

        if (settingsRegMap[SETTINGS_REG_CM108_IOMUX0] & SETTINGS_REG_CM108_IOMUX0_BTN1SRC_IN2_MASK) {
            USB_HIDSendButtonState(state & 0x01 ? USB_HID_BUTTON_VOLUP : 0x00);
        }

        if (settingsRegMap[SETTINGS_REG_CM108_IOMUX1] & SETTINGS_REG_CM108_IOMUX1_BTN2SRC_IN2_MASK) {
            USB_HIDSendButtonState(state & 0x01 ? USB_HID_BUTTON_VOLDN : 0x00);
        }

        if (settingsRegMap[SETTINGS_REG_CM108_IOMUX2] & SETTINGS_REG_CM108_IOMUX2_BTN3SRC_IN2_MASK) {
            USB_HIDSendButtonState(state & 0x01 ? USB_HID_BUTTON_PLAYMUTE : 0x00);
        }

        if (settingsRegMap[SETTINGS_REG_CM108_IOMUX3] & SETTINGS_REG_CM108_IOMUX3_BTN4SRC_IN2_MASK) {
            USB_HIDSendButtonState(state & 0x01 ? USB_HID_BUTTON_RECMUTE : 0x00);
        }

        if (settingsRegMap[SETTINGS_REG_SERIAL_IOMUX0] & SETTINGS_REG_SERIAL_IOMUX0_DCDSRC_IN2_MASK) {
            USB_SerialSendLineState(state & 0x01 ? USB_SERIAL_LINESTATE_DCD : 0x00);
        }

        if (settingsRegMap[SETTINGS_REG_SERIAL_IOMUX1] & SETTINGS_REG_SERIAL_IOMUX1_DSRSRC_IN2_MASK) {
            USB_SerialSendLineState(state & 0x01 ? USB_SERIAL_LINESTATE_DSR : 0x00);
        }

        if (settingsRegMap[SETTINGS_REG_SERIAL_IOMUX2] & SETTINGS_REG_SERIAL_IOMUX2_RISRC_IN2_MASK) {
            USB_SerialSendLineState(state & 0x01 ? USB_SERIAL_LINESTATE_RI : 0x00);
        }

        if (settingsRegMap[SETTINGS_REG_SERIAL_IOMUX3] & SETTINGS_REG_SERIAL_IOMUX3_BRKSRC_IN2_MASK) {
            USB_SerialSendLineState(state & 0x01 ? USB_SERIAL_LINESTATE_BREAK : 0x00);
        }
    }
}
