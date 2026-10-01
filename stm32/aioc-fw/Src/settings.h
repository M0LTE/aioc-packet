#ifndef SETTINGS_H_
#define SETTINGS_H_

#include <stdint.h>
#include "usb_descriptors.h"


#define SETTINGS_GET(REG, FIELD) ((settingsRegMap[REG] & (REG##_##FIELD##_##MASK)) >> (REG##_##FIELD##_##OFFS))

#define SETTINGS_REGMAP_SIZE     256
#define SETTINGS_REGMAP_READONLYADDR 0xC0

extern uint32_t settingsRegMap[SETTINGS_REGMAP_SIZE];

/* Magic number register. Mainly used to see if flash data is valid */
#define SETTINGS_REG_MAGIC                                  0x00
#define SETTINGS_REG_MAGIC_DEFAULT                          ( (((uint32_t) 'A') <<  0) | \
                                                              (((uint32_t) 'I') <<  8) | \
                                                              (((uint32_t) 'O') << 16) | \
                                                              (((uint32_t) 'C') << 24) )

/* USB ID register. The default USB VID and PID can be overwritten. Use with caution */
#define SETTINGS_REG_USBID                                  0x08
#define SETTINGS_REG_USBID_DEFAULT                          (SETTINGS_REG_USBID_VID_DFLT | SETTINGS_REG_USBID_PID_DFLT)
/* VID: USB Vendor Id */
#define SETTINGS_REG_USBID_VID_DFLT                         ((uint32_t) USB_VID << SETTINGS_REG_USBID_VID_OFFS)
#define SETTINGS_REG_USBID_VID_OFFS                         0
#define SETTINGS_REG_USBID_VID_MASK                         0x0000FFFFUL
/* PID: USB Product Id */
#define SETTINGS_REG_USBID_PID_DFLT                         ((uint32_t) USB_PID << SETTINGS_REG_USBID_PID_OFFS)
#define SETTINGS_REG_USBID_PID_OFFS                         16
#define SETTINGS_REG_USBID_PID_MASK                         0xFFFF0000UL

/* AIOC IOMUX0 register */
#define SETTINGS_REG_AIOC_IOMUX0                            0x24
#define SETTINGS_REG_AIOC_IOMUX0_DEFAULT                    (SETTINGS_REG_AIOC_IOMUX0_OUT1SRC_DFLT)
/* OUT1SRC: Source for OUT1 signal */
#define SETTINGS_REG_AIOC_IOMUX0_OUT1SRC_DFLT               (SETTINGS_REG_AIOC_IOMUX0_OUT1SRC_CM108GPIO3_MASK | SETTINGS_REG_AIOC_IOMUX0_OUT1SRC_SERIALDTRNRTS_MASK)
#define SETTINGS_REG_AIOC_IOMUX0_OUT1SRC_OFFS               0
#define SETTINGS_REG_AIOC_IOMUX0_OUT1SRC_MASK               0xFFFFFFFFUL
#define SETTINGS_REG_AIOC_IOMUX0_OUT1SRC_NONE_MASK          0x00000000UL
#define SETTINGS_REG_AIOC_IOMUX0_OUT1SRC_CM108GPIO1_MASK    0x00000001UL
#define SETTINGS_REG_AIOC_IOMUX0_OUT1SRC_CM108GPIO2_MASK    0x00000002UL
#define SETTINGS_REG_AIOC_IOMUX0_OUT1SRC_CM108GPIO3_MASK    0x00000004UL
#define SETTINGS_REG_AIOC_IOMUX0_OUT1SRC_CM108GPIO4_MASK    0x00000008UL
#define SETTINGS_REG_AIOC_IOMUX0_OUT1SRC_SERIALDTR_MASK     0x00000100UL
#define SETTINGS_REG_AIOC_IOMUX0_OUT1SRC_SERIALRTS_MASK     0x00000200UL
#define SETTINGS_REG_AIOC_IOMUX0_OUT1SRC_SERIALDTRNRTS_MASK 0x00000400UL
#define SETTINGS_REG_AIOC_IOMUX0_OUT1SRC_SERIALNDTRRTS_MASK 0x00000800UL
#define SETTINGS_REG_AIOC_IOMUX0_OUT1SRC_VPTT_MASK          0x00001000UL

/* AIOC IOMUX1 register */
#define SETTINGS_REG_AIOC_IOMUX1                            0x25
#define SETTINGS_REG_AIOC_IOMUX1_DEFAULT                    (SETTINGS_REG_AIOC_IOMUX1_OUT2SRC_DFLT)
/* OUT2SRC: Source for OUT2 signal */
#define SETTINGS_REG_AIOC_IOMUX1_OUT2SRC_DFLT               (SETTINGS_REG_AIOC_IOMUX1_OUT2SRC_CM108GPIO4_MASK)
#define SETTINGS_REG_AIOC_IOMUX1_OUT2SRC_OFFS               SETTINGS_REG_AIOC_IOMUX0_OUT1SRC_OFFS
#define SETTINGS_REG_AIOC_IOMUX1_OUT2SRC_MASK               SETTINGS_REG_AIOC_IOMUX0_OUT1SRC_MASK
#define SETTINGS_REG_AIOC_IOMUX1_OUT2SRC_NONE_MASK          SETTINGS_REG_AIOC_IOMUX0_OUT1SRC_NONE_MASK
#define SETTINGS_REG_AIOC_IOMUX1_OUT2SRC_CM108GPIO1_MASK    SETTINGS_REG_AIOC_IOMUX0_OUT1SRC_CM108GPIO1_MASK
#define SETTINGS_REG_AIOC_IOMUX1_OUT2SRC_CM108GPIO2_MASK    SETTINGS_REG_AIOC_IOMUX0_OUT1SRC_CM108GPIO2_MASK
#define SETTINGS_REG_AIOC_IOMUX1_OUT2SRC_CM108GPIO3_MASK    SETTINGS_REG_AIOC_IOMUX0_OUT1SRC_CM108GPIO3_MASK
#define SETTINGS_REG_AIOC_IOMUX1_OUT2SRC_CM108GPIO4_MASK    SETTINGS_REG_AIOC_IOMUX0_OUT1SRC_CM108GPIO4_MASK
#define SETTINGS_REG_AIOC_IOMUX1_OUT2SRC_SERIALDTR_MASK     SETTINGS_REG_AIOC_IOMUX0_OUT1SRC_SERIALDTR_MASK
#define SETTINGS_REG_AIOC_IOMUX1_OUT2SRC_SERIALRTS_MASK     SETTINGS_REG_AIOC_IOMUX0_OUT1SRC_SERIALRTS_MASK
#define SETTINGS_REG_AIOC_IOMUX1_OUT2SRC_SERIALDTRNRTS_MASK SETTINGS_REG_AIOC_IOMUX0_OUT1SRC_SERIALDTRNRTS_MASK
#define SETTINGS_REG_AIOC_IOMUX1_OUT2SRC_SERIALNDTRRTS_MASK SETTINGS_REG_AIOC_IOMUX0_OUT1SRC_SERIALNDTRRTS_MASK
#define SETTINGS_REG_AIOC_IOMUX1_OUT2SRC_VPTT_MASK          SETTINGS_REG_AIOC_IOMUX0_OUT1SRC_VPTT_MASK

/* CM108 IOMUX0 register */
#define SETTINGS_REG_CM108_IOMUX0                           0x44
#define SETTINGS_REG_CM108_IOMUX0_DEFAULT                   (SETTINGS_REG_CM108_IOMUX0_BTN1SRC_DFLT)
/* BTN1SRC: Volume-Up Button source */
#define SETTINGS_REG_CM108_IOMUX0_BTN1SRC_DFLT              (SETTINGS_REG_CM108_IOMUX0_BTN1SRC_IN2_MASK)
#define SETTINGS_REG_CM108_IOMUX0_BTN1SRC_OFFS              0
#define SETTINGS_REG_CM108_IOMUX0_BTN1SRC_MASK              0xFFFFFFFFUL
#define SETTINGS_REG_CM108_IOMUX0_BTN1SRC_NONE_MASK         0x00000000UL
#define SETTINGS_REG_CM108_IOMUX0_BTN1SRC_IN1_MASK          0x00010000UL /* AIOC's IN1 */
#define SETTINGS_REG_CM108_IOMUX0_BTN1SRC_IN2_MASK          0x00020000UL /* AIOC's IN2 */
#define SETTINGS_REG_CM108_IOMUX0_BTN1SRC_VCOS_MASK         0x01000000UL /* Virtual COS */

/* CM108 IOMUX1 register */
#define SETTINGS_REG_CM108_IOMUX1                           0x45
#define SETTINGS_REG_CM108_IOMUX1_DEFAULT                   (SETTINGS_REG_CM108_IOMUX1_BTN2SRC_DFLT)
/* BTN2SRC: Volume-Down Button source */
#define SETTINGS_REG_CM108_IOMUX1_BTN2SRC_DFLT              (SETTINGS_REG_CM108_IOMUX1_BTN2SRC_VCOS_MASK)
#define SETTINGS_REG_CM108_IOMUX1_BTN2SRC_OFFS              SETTINGS_REG_CM108_IOMUX0_BTN1SRC_OFFS
#define SETTINGS_REG_CM108_IOMUX1_BTN2SRC_MASK              SETTINGS_REG_CM108_IOMUX0_BTN1SRC_MASK
#define SETTINGS_REG_CM108_IOMUX1_BTN2SRC_NONE_MASK         SETTINGS_REG_CM108_IOMUX0_BTN1SRC_NONE_MASK
#define SETTINGS_REG_CM108_IOMUX1_BTN2SRC_IN1_MASK          SETTINGS_REG_CM108_IOMUX0_BTN1SRC_IN1_MASK
#define SETTINGS_REG_CM108_IOMUX1_BTN2SRC_IN2_MASK          SETTINGS_REG_CM108_IOMUX0_BTN1SRC_IN2_MASK
#define SETTINGS_REG_CM108_IOMUX1_BTN2SRC_VCOS_MASK         SETTINGS_REG_CM108_IOMUX0_BTN1SRC_VCOS_MASK

/* CM108 IOMUX2 register */
#define SETTINGS_REG_CM108_IOMUX2                           0x46
#define SETTINGS_REG_CM108_IOMUX2_DEFAULT                   (SETTINGS_REG_CM108_IOMUX2_BTN3SRC_DFLT)
/* BTN3SRC: Playback-Mute Button source */
#define SETTINGS_REG_CM108_IOMUX2_BTN3SRC_DFLT              (SETTINGS_REG_CM108_IOMUX2_BTN3SRC_NONE_MASK)
#define SETTINGS_REG_CM108_IOMUX2_BTN3SRC_OFFS              SETTINGS_REG_CM108_IOMUX0_BTN1SRC_OFFS
#define SETTINGS_REG_CM108_IOMUX2_BTN3SRC_MASK              SETTINGS_REG_CM108_IOMUX0_BTN1SRC_MASK
#define SETTINGS_REG_CM108_IOMUX2_BTN3SRC_NONE_MASK         SETTINGS_REG_CM108_IOMUX0_BTN1SRC_NONE_MASK
#define SETTINGS_REG_CM108_IOMUX2_BTN3SRC_IN1_MASK          SETTINGS_REG_CM108_IOMUX0_BTN1SRC_IN1_MASK
#define SETTINGS_REG_CM108_IOMUX2_BTN3SRC_IN2_MASK          SETTINGS_REG_CM108_IOMUX0_BTN1SRC_IN2_MASK
#define SETTINGS_REG_CM108_IOMUX2_BTN3SRC_VCOS_MASK         SETTINGS_REG_CM108_IOMUX0_BTN1SRC_VCOS_MASK

/* CM108 IOMUX3 register */
#define SETTINGS_REG_CM108_IOMUX3                           0x47
#define SETTINGS_REG_CM108_IOMUX3_DEFAULT                   (SETTINGS_REG_CM108_IOMUX3_BTN4SRC_DFLT)
/* BTN4SRC: Record-Mute Button source */
#define SETTINGS_REG_CM108_IOMUX3_BTN4SRC_DFLT              (SETTINGS_REG_CM108_IOMUX3_BTN4SRC_NONE_MASK)
#define SETTINGS_REG_CM108_IOMUX3_BTN4SRC_OFFS              SETTINGS_REG_CM108_IOMUX0_BTN1SRC_OFFS
#define SETTINGS_REG_CM108_IOMUX3_BTN4SRC_MASK              SETTINGS_REG_CM108_IOMUX0_BTN1SRC_MASK
#define SETTINGS_REG_CM108_IOMUX3_BTN4SRC_NONE_MASK         SETTINGS_REG_CM108_IOMUX0_BTN1SRC_NONE_MASK
#define SETTINGS_REG_CM108_IOMUX3_BTN4SRC_IN1_MASK          SETTINGS_REG_CM108_IOMUX0_BTN1SRC_IN1_MASK
#define SETTINGS_REG_CM108_IOMUX3_BTN4SRC_IN2_MASK          SETTINGS_REG_CM108_IOMUX0_BTN1SRC_IN2_MASK
#define SETTINGS_REG_CM108_IOMUX3_BTN4SRC_VCOS_MASK         SETTINGS_REG_CM108_IOMUX0_BTN1SRC_VCOS_MASK

/* Serial (CDC) Control register */
#define SETTINGS_REG_SERIAL_CTRL                            0x60
#define SETTINGS_REG_SERIAL_CTRL_DEFAULT                    (SETTINGS_REG_SERIAL_CTRL_TXFRCPTT_DFLT | SETTINGS_REG_SERIAL_CTRL_RXIGNPTT_DFLT)
/* TXFRCPTT: Forces PTT signal(s) to zero when transmitting serial data to radio if enabled */
#define SETTINGS_REG_SERIAL_CTRL_TXFRCPTT_DFLT              (SETTINGS_REG_SERIAL_CTRL_TXFRCPTT_PTT1_MASK)
#define SETTINGS_REG_SERIAL_CTRL_TXFRCPTT_OFFS              8
#define SETTINGS_REG_SERIAL_CTRL_TXFRCPTT_MASK              0x00000F00UL
#define SETTINGS_REG_SERIAL_CTRL_TXFRCPTT_NONE_MASK         0x00000000UL
#define SETTINGS_REG_SERIAL_CTRL_TXFRCPTT_PTT1_MASK         0x00000100UL
#define SETTINGS_REG_SERIAL_CTRL_TXFRCPTT_PTT2_MASK         0x00000200UL
/* RXIGNPTT: Ignores reception of data from radio when PTT signal(s) asserted if enabled */
#define SETTINGS_REG_SERIAL_CTRL_RXIGNPTT_DFLT              (SETTINGS_REG_SERIAL_CTRL_RXIGNPTT_PTT1_MASK)
#define SETTINGS_REG_SERIAL_CTRL_RXIGNPTT_OFFS              16
#define SETTINGS_REG_SERIAL_CTRL_RXIGNPTT_MASK              0x000F0000UL
#define SETTINGS_REG_SERIAL_CTRL_RXIGNPTT_NONE_MASK         0x00000000UL
#define SETTINGS_REG_SERIAL_CTRL_RXIGNPTT_PTT1_MASK         0x00010000UL
#define SETTINGS_REG_SERIAL_CTRL_RXIGNPTT_PTT2_MASK         0x00020000UL

/* Serial (CDC) IOMUX0 register */
#define SETTINGS_REG_SERIAL_IOMUX0                          0x64
#define SETTINGS_REG_SERIAL_IOMUX0_DEFAULT                  (SETTINGS_REG_SERIAL_IOMUX0_DCDSRC_DFLT)
/* DCDSRC: DCD (Data Carrier Detect) signal source */
#define SETTINGS_REG_SERIAL_IOMUX0_DCDSRC_DFLT              (SETTINGS_REG_SERIAL_IOMUX0_DCDSRC_VCOS_MASK)
#define SETTINGS_REG_SERIAL_IOMUX0_DCDSRC_OFFS              SETTINGS_REG_CM108_IOMUX0_BTN1SRC_OFFS
#define SETTINGS_REG_SERIAL_IOMUX0_DCDSRC_MASK              SETTINGS_REG_CM108_IOMUX0_BTN1SRC_MASK
#define SETTINGS_REG_SERIAL_IOMUX0_DCDSRC_NONE_MASK         SETTINGS_REG_CM108_IOMUX0_BTN1SRC_NONE_MASK
#define SETTINGS_REG_SERIAL_IOMUX0_DCDSRC_IN1_MASK          SETTINGS_REG_CM108_IOMUX0_BTN1SRC_IN1_MASK
#define SETTINGS_REG_SERIAL_IOMUX0_DCDSRC_IN2_MASK          SETTINGS_REG_CM108_IOMUX0_BTN1SRC_IN2_MASK
#define SETTINGS_REG_SERIAL_IOMUX0_DCDSRC_VCOS_MASK         SETTINGS_REG_CM108_IOMUX0_BTN1SRC_VCOS_MASK

/* Serial (CDC) IOMUX1 register */
#define SETTINGS_REG_SERIAL_IOMUX1                          0x65
#define SETTINGS_REG_SERIAL_IOMUX1_DEFAULT                  (SETTINGS_REG_SERIAL_IOMUX1_DSRSRC_DFLT)
/* DSRSRC: DSR (Data Set Ready) signal source */
#define SETTINGS_REG_SERIAL_IOMUX1_DSRSRC_DFLT              (SETTINGS_REG_SERIAL_IOMUX1_DSRSRC_NONE_MASK)
#define SETTINGS_REG_SERIAL_IOMUX1_DSRSRC_OFFS              SETTINGS_REG_SERIAL_IOMUX0_DCDSRC_OFFS
#define SETTINGS_REG_SERIAL_IOMUX1_DSRSRC_MASK              SETTINGS_REG_SERIAL_IOMUX0_DCDSRC_MASK
#define SETTINGS_REG_SERIAL_IOMUX1_DSRSRC_NONE_MASK         SETTINGS_REG_SERIAL_IOMUX0_DCDSRC_NONE_MASK
#define SETTINGS_REG_SERIAL_IOMUX1_DSRSRC_IN1_MASK          SETTINGS_REG_SERIAL_IOMUX0_DCDSRC_IN1_MASK
#define SETTINGS_REG_SERIAL_IOMUX1_DSRSRC_IN2_MASK          SETTINGS_REG_SERIAL_IOMUX0_DCDSRC_IN2_MASK
#define SETTINGS_REG_SERIAL_IOMUX1_DSRSRC_VCOS_MASK         SETTINGS_REG_SERIAL_IOMUX0_DCDSRC_VCOS_MASK

/* Serial (CDC) IOMUX2 register */
#define SETTINGS_REG_SERIAL_IOMUX2                          0x66
#define SETTINGS_REG_SERIAL_IOMUX2_DEFAULT                  (SETTINGS_REG_SERIAL_IOMUX2_RISRC_DFLT)
/* RISRC: RI (Ring Indicator) signal source */
#define SETTINGS_REG_SERIAL_IOMUX2_RISRC_DFLT               (SETTINGS_REG_SERIAL_IOMUX2_RISRC_NONE_MASK)
#define SETTINGS_REG_SERIAL_IOMUX2_RISRC_OFFS               SETTINGS_REG_SERIAL_IOMUX0_DCDSRC_OFFS
#define SETTINGS_REG_SERIAL_IOMUX2_RISRC_MASK               SETTINGS_REG_SERIAL_IOMUX0_DCDSRC_MASK
#define SETTINGS_REG_SERIAL_IOMUX2_RISRC_NONE_MASK          SETTINGS_REG_SERIAL_IOMUX0_DCDSRC_NONE_MASK
#define SETTINGS_REG_SERIAL_IOMUX2_RISRC_IN1_MASK           SETTINGS_REG_SERIAL_IOMUX0_DCDSRC_IN1_MASK
#define SETTINGS_REG_SERIAL_IOMUX2_RISRC_IN2_MASK           SETTINGS_REG_SERIAL_IOMUX0_DCDSRC_IN2_MASK
#define SETTINGS_REG_SERIAL_IOMUX2_RISRC_VCOS_MASK          SETTINGS_REG_SERIAL_IOMUX0_DCDSRC_VCOS_MASK

/* Serial (CDC) IOMUX3 register */
#define SETTINGS_REG_SERIAL_IOMUX3                          0x67
#define SETTINGS_REG_SERIAL_IOMUX3_DEFAULT                  (SETTINGS_REG_SERIAL_IOMUX3_BRKSRC_DFLT)
/* BRKSRC: BREAK (Break) signal source */
#define SETTINGS_REG_SERIAL_IOMUX3_BRKSRC_DFLT              (SETTINGS_REG_SERIAL_IOMUX3_BRKSRC_NONE_MASK)
#define SETTINGS_REG_SERIAL_IOMUX3_BRKSRC_OFFS              SETTINGS_REG_SERIAL_IOMUX0_DCDSRC_OFFS
#define SETTINGS_REG_SERIAL_IOMUX3_BRKSRC_MASK              SETTINGS_REG_SERIAL_IOMUX0_DCDSRC_MASK
#define SETTINGS_REG_SERIAL_IOMUX3_BRKSRC_NONE_MASK         SETTINGS_REG_SERIAL_IOMUX0_DCDSRC_NONE_MASK
#define SETTINGS_REG_SERIAL_IOMUX3_BRKSRC_IN1_MASK          SETTINGS_REG_SERIAL_IOMUX0_DCDSRC_IN1_MASK
#define SETTINGS_REG_SERIAL_IOMUX3_BRKSRC_IN2_MASK          SETTINGS_REG_SERIAL_IOMUX0_DCDSRC_IN2_MASK
#define SETTINGS_REG_SERIAL_IOMUX3_BRKSRC_VCOS_MASK         SETTINGS_REG_SERIAL_IOMUX0_DCDSRC_VCOS_MASK

/* Audio RX settings register */
#define SETTINGS_REG_AUDIO_RX                               0x72
#define SETTINGS_REG_AUDIO_RX_DEFAULT                       SETTINGS_REG_AUDIO_RX_RXGAIN_DFLT
/* RXGAIN: Sets the RX gain of the audio input. Requires HW version >= 1.2 */
#define SETTINGS_REG_AUDIO_RX_RXGAIN_DFLT                   SETTINGS_REG_AUDIO_RX_RXGAIN_1X_ENUM
#define SETTINGS_REG_AUDIO_RX_RXGAIN_OFFS                   16
#define SETTINGS_REG_AUDIO_RX_RXGAIN_MASK                   0x000F0000UL
#define SETTINGS_REG_AUDIO_RX_RXGAIN_1X_ENUM                0x0
#define SETTINGS_REG_AUDIO_RX_RXGAIN_2X_ENUM                0x1
#define SETTINGS_REG_AUDIO_RX_RXGAIN_4X_ENUM                0x2
#define SETTINGS_REG_AUDIO_RX_RXGAIN_8X_ENUM                0x3
#define SETTINGS_REG_AUDIO_RX_RXGAIN_16X_ENUM               0x4

/* Audio TX settings register */
#define SETTINGS_REG_AUDIO_TX                               0x78
#define SETTINGS_REG_AUDIO_TX_DEFAULT                       0
/* TXBOOST: Defines the audio output level. Either MIC level or LINE level (boosted). Requires HW version >= 1.2 */
#define SETTINGS_REG_AUDIO_TX_TXBOOST_OFFS                  8
#define SETTINGS_REG_AUDIO_TX_TXBOOST_MASK                  (1UL << SETTINGS_REG_AUDIO_TX_TXBOOST_OFFS)

/* Virtual PTT level control register */
#define SETTINGS_REG_VPTT_LVLCTRL                           0x82
#define SETTINGS_REG_VPTT_LVLCTRL_DEFAULT                   (SETTINGS_REG_VPTT_LVLCTRL_THRSHLD_DFLT)
/* THRSHLD: Virtual PTT threshold level */
#define SETTINGS_REG_VPTT_LVLCTRL_THRSHLD_DFLT              ((uint32_t) 16 << SETTINGS_REG_VPTT_LVLCTRL_THRSHLD_OFFS)
#define SETTINGS_REG_VPTT_LVLCTRL_THRSHLD_OFFS              0
#define SETTINGS_REG_VPTT_LVLCTRL_THRSHLD_MASK              0x0000FFFFUL

/* Virtual PTT timing control register */
#define SETTINGS_REG_VPTT_TIMCTRL                           0x84
#define SETTINGS_REG_VPTT_TIMCTRL_DEFAULT                   (SETTINGS_REG_VPTT_TIMCTRL_TIMEOUT_DFLT)
/* TIMEOUT: Timeout (trailing) time in milliseconds in 12.4 format */
#define SETTINGS_REG_VPTT_TIMCTRL_TIMEOUT_DFLT              ((uint32_t) (20 << 4) << SETTINGS_REG_VPTT_TIMCTRL_TIMEOUT_OFFS)
#define SETTINGS_REG_VPTT_TIMCTRL_TIMEOUT_OFFS              0
#define SETTINGS_REG_VPTT_TIMCTRL_TIMEOUT_MASK              0xFFFFFFFFUL

/* Virtual COS level control register */
#define SETTINGS_REG_VCOS_LVLCTRL                           0x92
#define SETTINGS_REG_VCOS_LVLCTRL_DEFAULT                   (SETTINGS_REG_VCOS_LVLCTRL_THRSHLD_DFLT)
/* THRSHLD: Virtual COS threshold level */
#define SETTINGS_REG_VCOS_LVLCTRL_THRSHLD_DFLT              ((uint32_t) 256 << SETTINGS_REG_VCOS_LVLCTRL_THRSHLD_OFFS)
#define SETTINGS_REG_VCOS_LVLCTRL_THRSHLD_OFFS              0
#define SETTINGS_REG_VCOS_LVLCTRL_THRSHLD_MASK              0x0000FFFFUL

/* Virtual COS timing control register */
#define SETTINGS_REG_VCOS_TIMCTRL                           0x94
#define SETTINGS_REG_VCOS_TIMCTRL_DEFAULT                   (SETTINGS_REG_VCOS_TIMCTRL_TIMEOUT_DFLT)
/* TIMEOUT: Timeout (trailing) time in milliseconds in 12.4 format  */
#define SETTINGS_REG_VCOS_TIMCTRL_TIMEOUT_DFLT              ((uint32_t) (200 << 4) << SETTINGS_REG_VCOS_TIMCTRL_TIMEOUT_OFFS)
#define SETTINGS_REG_VCOS_TIMCTRL_TIMEOUT_OFFS              0
#define SETTINGS_REG_VCOS_TIMCTRL_TIMEOUT_MASK              0x0000FFFFUL

/* Fox Hunt settings register */
#define SETTINGS_REG_FOXHUNT_CTRL                           0xA0
#define SETTINGS_REG_FOXHUNT_CTRL_DEFAULT                   (SETTINGS_REG_FOXHUNT_CTRL_INTERVAL_DFLT | SETTINGS_REG_FOXHUNT_CTRL_WPM_DFLT | SETTINGS_REG_FOXHUNT_CTRL_VOLUME_DFLT)
/* INTERVAL: Beacon interval in seconds */
#define SETTINGS_REG_FOXHUNT_CTRL_INTERVAL_DFLT             ((uint32_t) 0 << SETTINGS_REG_FOXHUNT_CTRL_INTERVAL_OFFS)
#define SETTINGS_REG_FOXHUNT_CTRL_INTERVAL_OFFS             0
#define SETTINGS_REG_FOXHUNT_CTRL_INTERVAL_MASK             0x000000FFUL
/* WPM: Words per Minute morse speed */
#define SETTINGS_REG_FOXHUNT_CTRL_WPM_DFLT                  ((uint32_t) 20 << SETTINGS_REG_FOXHUNT_CTRL_WPM_OFFS)
#define SETTINGS_REG_FOXHUNT_CTRL_WPM_OFFS                  8
#define SETTINGS_REG_FOXHUNT_CTRL_WPM_MASK                  0x0000FF00UL
/* VOLUME: Transmit volume */
#define SETTINGS_REG_FOXHUNT_CTRL_VOLUME_DFLT               ((uint32_t) 32768 << SETTINGS_REG_FOXHUNT_CTRL_VOLUME_OFFS)
#define SETTINGS_REG_FOXHUNT_CTRL_VOLUME_OFFS               16
#define SETTINGS_REG_FOXHUNT_CTRL_VOLUME_MASK               0xFFFF0000UL

/* Fox Hunt message 0 register */
#define SETTINGS_REG_FOXHUNT_MSG0                           0xA2
#define SETTINGS_REG_FOXHUNT_MSG0_DEFAULT                   0
#define SETTINGS_REG_FOXHUNT_MSG0_CHAR00_OFFS               0
#define SETTINGS_REG_FOXHUNT_MSG0_CHAR00_MASK               0x000000FFUL
#define SETTINGS_REG_FOXHUNT_MSG0_CHAR01_OFFS               8
#define SETTINGS_REG_FOXHUNT_MSG0_CHAR01_MASK               0x0000FF00UL
#define SETTINGS_REG_FOXHUNT_MSG0_CHAR02_OFFS               16
#define SETTINGS_REG_FOXHUNT_MSG0_CHAR02_MASK               0x00FF0000UL
#define SETTINGS_REG_FOXHUNT_MSG0_CHAR03_OFFS               24
#define SETTINGS_REG_FOXHUNT_MSG0_CHAR03_MASK               0xFF000000UL

/* Fox Hunt message 1 register */
#define SETTINGS_REG_FOXHUNT_MSG1                           0xA3
#define SETTINGS_REG_FOXHUNT_MSG1_DEFAULT                   0
#define SETTINGS_REG_FOXHUNT_MSG1_CHAR04_OFFS               0
#define SETTINGS_REG_FOXHUNT_MSG1_CHAR04_MASK               0x000000FFUL
#define SETTINGS_REG_FOXHUNT_MSG1_CHAR05_OFFS               8
#define SETTINGS_REG_FOXHUNT_MSG1_CHAR05_MASK               0x0000FF00UL
#define SETTINGS_REG_FOXHUNT_MSG1_CHAR06_OFFS               16
#define SETTINGS_REG_FOXHUNT_MSG1_CHAR06_MASK               0x00FF0000UL
#define SETTINGS_REG_FOXHUNT_MSG1_CHAR07_OFFS               24
#define SETTINGS_REG_FOXHUNT_MSG1_CHAR07_MASK               0xFF000000UL

/* Fox Hunt message 2 register */
#define SETTINGS_REG_FOXHUNT_MSG2                           0xA4
#define SETTINGS_REG_FOXHUNT_MSG2_DEFAULT                   0
#define SETTINGS_REG_FOXHUNT_MSG2_CHAR08_OFFS               0
#define SETTINGS_REG_FOXHUNT_MSG2_CHAR08_MASK               0x000000FFUL
#define SETTINGS_REG_FOXHUNT_MSG2_CHAR09_OFFS               8
#define SETTINGS_REG_FOXHUNT_MSG2_CHAR09_MASK               0x0000FF00UL
#define SETTINGS_REG_FOXHUNT_MSG2_CHAR10_OFFS               16
#define SETTINGS_REG_FOXHUNT_MSG2_CHAR10_MASK               0x00FF0000UL
#define SETTINGS_REG_FOXHUNT_MSG2_CHAR11_OFFS               24
#define SETTINGS_REG_FOXHUNT_MSG2_CHAR11_MASK               0xFF000000UL

/* Fox Hunt message 3 register */
#define SETTINGS_REG_FOXHUNT_MSG3                           0xA5
#define SETTINGS_REG_FOXHUNT_MSG3_DEFAULT                   0
#define SETTINGS_REG_FOXHUNT_MSG3_CHAR12_OFFS               0
#define SETTINGS_REG_FOXHUNT_MSG3_CHAR12_MASK               0x000000FFUL
#define SETTINGS_REG_FOXHUNT_MSG3_CHAR13_OFFS               8
#define SETTINGS_REG_FOXHUNT_MSG3_CHAR13_MASK               0x0000FF00UL
#define SETTINGS_REG_FOXHUNT_MSG3_CHAR14_OFFS               16
#define SETTINGS_REG_FOXHUNT_MSG3_CHAR14_MASK               0x00FF0000UL
#define SETTINGS_REG_FOXHUNT_MSG3_CHAR15_OFFS               24
#define SETTINGS_REG_FOXHUNT_MSG3_CHAR15_MASK               0xFF000000UL

/* RX equaliser control register (see bench/EQ.md). Picks the equaliser in the recording
 * (ADC -> USB IN) path, applied after the virtual COS level check and before the volume:
 * 0 = off, the samples pass through untouched as in v1.4.1; 1 = the built-in UV-K5 profile,
 * which undoes the K5's receive high-pass (about 128 Hz, second order) in magnitude and phase
 * so that 9600 baud FSK decodes, at the cost of 336 samples (7.0 ms) of extra delay. Any
 * other value is reserved and bypasses. Runs only while the host records at 48000 Hz. A
 * change takes effect at the next 1 ms block: switching on, the filter first runs unheard for
 * 1024 samples to fill its delay lines, then both switching on and off crossfade over 512
 * samples. Unused in v1.4.1 and every upstream branch, so a page stored by them (or by an
 * aioc-packet without the RX equaliser) holds 0 here; see SETTINGS_REG_INFO_RXEQPAGE. */
#define SETTINGS_REG_RXEQ_CTRL                              0xAF
#ifndef TXEQ_DEFAULT_OFF
/* Default: the UV-K5 profile, on. Used when nothing is stored, for a page stored by firmware
 * without the RX equaliser, and on a "load defaults" request */
#define SETTINGS_REG_RXEQ_CTRL_DEFAULT                      (SETTINGS_REG_RXEQ_CTRL_PROFILE_K5_ENUM << SETTINGS_REG_RXEQ_CTRL_PROFILE_OFFS)
#else
/* Developer build option (make TXEQ_DEFAULT=off): both equalisers default to off */
#define SETTINGS_REG_RXEQ_CTRL_DEFAULT                      (SETTINGS_REG_RXEQ_CTRL_PROFILE_OFF_ENUM << SETTINGS_REG_RXEQ_CTRL_PROFILE_OFFS)
#endif
/* PROFILE: the whole word. Values other than the ones below are reserved (bypass) */
#define SETTINGS_REG_RXEQ_CTRL_PROFILE_OFFS                 0
#define SETTINGS_REG_RXEQ_CTRL_PROFILE_MASK                 0xFFFFFFFFUL
#define SETTINGS_REG_RXEQ_CTRL_PROFILE_OFF_ENUM             0x0UL
#define SETTINGS_REG_RXEQ_CTRL_PROFILE_K5_ENUM              0x1UL

/* TX equaliser coefficients (packet-eq branch, see bench/EQ.md).
 * Biquad cascade in the playback (USB OUT -> DAC) path. Section k (0..2) at 0xB0 + 5*k holds
 * b0, b1, b2, a1, a2, each signed 32 bit Q3.29 (1.0 = 0x20000000), for
 * y = b0 x + b1 x1 + b2 x2 - a1 y1 - a2 y2.
 * Writing these has no effect until SETTINGS_REG_TXEQ_CTRL changes value. */
#define SETTINGS_REG_TXEQ_COEF0                             0xB0
#define SETTINGS_REG_TXEQ_COEF_COUNT                        15

/* TX equaliser control register. All zero = bypass, identical to v1.4.1.
 * The coefficients are copied, all together between two DAC samples, whenever this register
 * changes value: write the coefficients first, then this register with a new GEN. The new set
 * then settles unheard for 4096 samples and crossfades in over 512 (enable and disable too).
 * It sits above the coefficients so that a flash recall (which copies upwards) sets it last. */
#define SETTINGS_REG_TXEQ_CTRL                              0xBF
#ifndef TXEQ_DEFAULT_OFF
/* Default: the measured "K5 on red v1 AIOC" profile, the same values tools/aioc_eq.py writes
 * for "apply k5-red". NSECT 3, GEN 0x13, FS 48000. Defaults are used when the settings page
 * holds nothing stored (after flashing a full image), for the EQ registers of a page stored
 * by firmware without the equaliser (see SETTINGS_REG_INFO_TXEQPAGE), and on a "load
 * defaults" request. */
#define SETTINGS_REG_TXEQ_CTRL_DEFAULT                      0xBB801303UL
#define SETTINGS_REG_TXEQ_COEF_DEFAULTS { \
    0x1FC74BD8UL, 0xC0E42580UL, 0x1F551D69UL, 0xC0E42580UL, 0x1F1C6941UL, \
    0x12B8B2CDUL, 0xF090A649UL, 0x04B022DFUL, 0xDF2C6FBFUL, 0x11C94E12UL, \
    0x2029FDF6UL, 0xC3FC2B77UL, 0x1C737807UL, 0xC3FC2B77UL, 0x1C9D75FCUL }
#else
/* Developer build option (make TXEQ_DEFAULT=off): the equaliser defaults to off, so with
 * nothing stored the audio is bit-for-bit v1.4.1 */
#define SETTINGS_REG_TXEQ_CTRL_DEFAULT                      0
#define SETTINGS_REG_TXEQ_COEF_DEFAULTS { \
    0, 0, 0, 0, 0, \
    0, 0, 0, 0, 0, \
    0, 0, 0, 0, 0 }
#endif
/* NSECT: Number of biquad sections in use (0 = bypass, 1..3) */
#define SETTINGS_REG_TXEQ_CTRL_NSECT_OFFS                   0
#define SETTINGS_REG_TXEQ_CTRL_NSECT_MASK                   0x00000003UL
/* GEN: Commit tag. Change it to latch a new coefficient set */
#define SETTINGS_REG_TXEQ_CTRL_GEN_OFFS                     8
#define SETTINGS_REG_TXEQ_CTRL_GEN_MASK                     0x0000FF00UL
/* FS: Sample rate in Hz the coefficients were designed for. The EQ is bypassed while playback
 * runs at any other rate. 0 = apply at any rate */
#define SETTINGS_REG_TXEQ_CTRL_FS_OFFS                      16
#define SETTINGS_REG_TXEQ_CTRL_FS_MASK                      0xFFFF0000UL

/* AIOC debug register 0 */
#define SETTINGS_REG_INFO_AIOC0                             0xC0
#define SETTINGS_REG_INFO_AIOC0_DEFAULT                     0
/* Various digital signal states: the PTT outputs, virtual PTT and virtual COS. (Upstream
 * writes these into SETTINGS_REG_INFO_AUDIO0 instead, over its record mute bits and its
 * record and play state fields; this firmware writes them here) */
#define SETTINGS_REG_INFO_AIOC0_PTT1STATE_MASK              0x00010000UL
#define SETTINGS_REG_INFO_AIOC0_PTT2STATE_MASK              0x00020000UL
#define SETTINGS_REG_INFO_AIOC0_VPTTSTATE_MASK              0x01000000UL
#define SETTINGS_REG_INFO_AIOC0_VCOSSTATE_MASK              0x10000000UL

/* TX equaliser status register (read only) */
#define SETTINGS_REG_INFO_TXEQ                              0xC8
#define SETTINGS_REG_INFO_TXEQ_DEFAULT                      0
/* Sections in use, filter running (NSECT > 0 and sample rate matches), a change is settling or
 * crossfading, GEN of the set in use, and the number of saturation events since the last commit
 * (sticks at 0xFFFF). Updated at every playback block (1 ms), so only while playback is running */
#define SETTINGS_REG_INFO_TXEQ_NSECT_OFFS                   0
#define SETTINGS_REG_INFO_TXEQ_NSECT_MASK                   0x00000003UL
#define SETTINGS_REG_INFO_TXEQ_ACTIVE_MASK                  0x00000004UL
#define SETTINGS_REG_INFO_TXEQ_FADE_MASK                    0x00000008UL
#define SETTINGS_REG_INFO_TXEQ_GEN_OFFS                     8
#define SETTINGS_REG_INFO_TXEQ_GEN_MASK                     0x0000FF00UL
#define SETTINGS_REG_INFO_TXEQ_CLIPS_OFFS                   16
#define SETTINGS_REG_INFO_TXEQ_CLIPS_MASK                   0xFFFF0000UL

/* TX equaliser settings page marker (read only). Always "TXEQ" in RAM on this firmware, so
 * every settings page it stores carries it. Firmware without the equaliser (stock v1.4.x)
 * leaves this address zero. On recall, the EQ registers come from the stored page if the
 * page carries the marker or a nonzero TXEQ_CTRL (a page stored by an earlier aioc-packet
 * with the EQ on); otherwise the page predates the equaliser and they get their defaults */
#define SETTINGS_REG_INFO_TXEQPAGE                          0xC9
#define SETTINGS_REG_INFO_TXEQPAGE_DEFAULT                  SETTINGS_REG_INFO_TXEQPAGE_MARKER
#define SETTINGS_REG_INFO_TXEQPAGE_MARKER                   ( (((uint32_t) 'T') <<  0) | \
                                                              (((uint32_t) 'X') <<  8) | \
                                                              (((uint32_t) 'E') << 16) | \
                                                              (((uint32_t) 'Q') << 24) )

/* RX equaliser status register (read only) */
#define SETTINGS_REG_INFO_RXEQ                              0xCA
#define SETTINGS_REG_INFO_RXEQ_DEFAULT                      0
/* Profile in use (0 while off or at another sample rate), filter running (it is heard: a
 * profile is in use, so recording runs at 48000 Hz), a change is settling or crossfading,
 * and the number of output samples saturated to int16 since recording started or the last
 * change (sticks at 0xFFFF). Copied from the equaliser by the main loop; it keeps the last
 * recording's values while nothing records */
#define SETTINGS_REG_INFO_RXEQ_PROFILE_OFFS                 0
#define SETTINGS_REG_INFO_RXEQ_PROFILE_MASK                 0x000000FFUL
#define SETTINGS_REG_INFO_RXEQ_ACTIVE_MASK                  0x00000100UL
#define SETTINGS_REG_INFO_RXEQ_FADE_MASK                    0x00000200UL
/* The overload guard switched the equaliser off (bit 10, until recording restarts), and how
 * many times it has done so since the AIOC last started, at power-up or any reset (bits
 * 11-15, sticks at 31). It trips when the main loop gets no CPU time for 50 ms while the
 * equaliser runs, or the equaliser takes over 600 cycles per sample for 1 ms */
#define SETTINGS_REG_INFO_RXEQ_OVERLOAD_MASK                0x00000400UL
#define SETTINGS_REG_INFO_RXEQ_OVERLOADS_OFFS               11
#define SETTINGS_REG_INFO_RXEQ_OVERLOADS_MASK               0x0000F800UL
#define SETTINGS_REG_INFO_RXEQ_CLIPS_OFFS                   16
#define SETTINGS_REG_INFO_RXEQ_CLIPS_MASK                   0xFFFF0000UL

/* RX equaliser settings page marker (read only). Always "RXEQ" in RAM on this firmware, so
 * every settings page it stores carries it; earlier firmware (stock, and aioc-packet up to
 * v1.4.1-packet.2) leaves this address zero. On recall, RXEQ_CTRL comes from the stored page
 * only if the page carries the marker; otherwise it gets its default */
#define SETTINGS_REG_INFO_RXEQPAGE                          0xCB
#define SETTINGS_REG_INFO_RXEQPAGE_DEFAULT                  SETTINGS_REG_INFO_RXEQPAGE_MARKER
#define SETTINGS_REG_INFO_RXEQPAGE_MARKER                   ( (((uint32_t) 'R') <<  0) | \
                                                              (((uint32_t) 'X') <<  8) | \
                                                              (((uint32_t) 'E') << 16) | \
                                                              (((uint32_t) 'Q') << 24) )

/* RX equaliser CPU cost (read only): cycles spent on the equaliser per ADC sample, measured
 * with the DWT cycle counter around the equaliser in each 1 ms block and divided by the block
 * length (72 cycles = 1 us; one sample period at 48 kHz is 1500 cycles). The maximum since
 * recording started (the most expensive block's average, sticks at 0xFFFF) and the average
 * over the last 4096 samples or more. Copied like SETTINGS_REG_INFO_RXEQ */
#define SETTINGS_REG_INFO_RXEQCYC                           0xCC
#define SETTINGS_REG_INFO_RXEQCYC_DEFAULT                   0
#define SETTINGS_REG_INFO_RXEQCYC_MAX_OFFS                  0
#define SETTINGS_REG_INFO_RXEQCYC_MAX_MASK                  0x0000FFFFUL
#define SETTINGS_REG_INFO_RXEQCYC_AVG_OFFS                  16
#define SETTINGS_REG_INFO_RXEQCYC_AVG_MASK                  0xFFFF0000UL

/* Audio CPU cost (read only): cycles the recording (RX) block interrupt takes per block, from
 * the ADC buffer to USB (VCOS check, RX equaliser, volume), measured with the DWT cycle
 * counter. A block is 1 ms of audio (48 samples at 48 kHz, 72000 cycles of time). The maximum
 * since recording started (sticks at 0xFFFF) and the average over the last 4096 samples or
 * more. Updated at every block, so only while recording runs; zero after power-up */
#define SETTINGS_REG_INFO_RXCYC                             0xCD
#define SETTINGS_REG_INFO_RXCYC_DEFAULT                     0
#define SETTINGS_REG_INFO_RXCYC_MAX_OFFS                    0
#define SETTINGS_REG_INFO_RXCYC_MAX_MASK                    0x0000FFFFUL
#define SETTINGS_REG_INFO_RXCYC_AVG_OFFS                    16
#define SETTINGS_REG_INFO_RXCYC_AVG_MASK                    0xFFFF0000UL

/* Audio CPU cost (read only): cycles the playback (TX) block interrupt takes per block, from
 * USB to the DAC buffer (VPTT check, volume, TX equaliser), measured with the DWT cycle
 * counter. A block is 1 ms of audio (48 samples at 48 kHz, 72000 cycles of time). The maximum
 * since playback started (sticks at 0xFFFF) and the average over the last 4096 samples or
 * more. Updated at every block, so only while playback runs; zero after power-up */
#define SETTINGS_REG_INFO_TXCYC                             0xCE
#define SETTINGS_REG_INFO_TXCYC_DEFAULT                     0
#define SETTINGS_REG_INFO_TXCYC_MAX_OFFS                    0
#define SETTINGS_REG_INFO_TXCYC_MAX_MASK                    0x0000FFFFUL
#define SETTINGS_REG_INFO_TXCYC_AVG_OFFS                    16
#define SETTINGS_REG_INFO_TXCYC_AVG_MASK                    0xFFFF0000UL

/* Reset diagnostics (read only, see diag.h and tools/aioc_eq.py diag): why the AIOC last
 * reset. Taken at boot from a RAM record that survives every reset but a power-on, and put
 * back after every recall or "load defaults". 15 registers, unused in v1.4.1 and every
 * upstream branch */
#define SETTINGS_REG_INFO_DIAG                              0xE0
#define SETTINGS_REG_INFO_DIAG_COUNT                        15
/* 0xE0: marker, always "DIAG" (0x47414944) on firmware that has these registers */
#define SETTINGS_REG_INFO_DIAG_MARKER                       ( (((uint32_t) 'D') <<  0) | \
                                                              (((uint32_t) 'I') <<  8) | \
                                                              (((uint32_t) 'A') << 16) | \
                                                              (((uint32_t) 'G') << 24) )
/* 0xE1 DIAGRESET: resets since power-on (bits 0-15, sticks at 0xFFFF), a crash record is
 * present (bit 16), the host asked for the reboot (bit 17), the RAM record survived so the
 * counts, breadcrumbs and crash record mean something (bit 18), and RCC->CSR as read at boot
 * (bits 23-31 in place: 23 V18PWRRSTF, 25 OBLRSTF, 26 PINRSTF, 27 PORRSTF, 28 SFTRSTF,
 * 29 IWDGRSTF, 30 WWDGRSTF, 31 LPWRRSTF) */
#define SETTINGS_REG_INFO_DIAGRESET                         0xE1
#define SETTINGS_REG_INFO_DIAGRESET_COUNT_MASK              0x0000FFFFUL
#define SETTINGS_REG_INFO_DIAGRESET_FAULT_MASK              0x00010000UL
#define SETTINGS_REG_INFO_DIAGRESET_REBOOT_MASK             0x00020000UL
#define SETTINGS_REG_INFO_DIAGRESET_VALID_MASK              0x00040000UL
#define SETTINGS_REG_INFO_DIAGRESET_CSR_MASK                0xFF800000UL
/* 0xE2 DIAGFAULT: the exception that recorded the crash (bits 0-8, IPSR: 3 HardFault,
 * 4 MemManage, 5 BusFault, 6 UsageFault, 16 and up an unexpected interrupt, IRQ n - 16) and
 * faults since power-on (bits 16-31) */
#define SETTINGS_REG_INFO_DIAGFAULT                         0xE2
#define SETTINGS_REG_INFO_DIAGFAULT_EXC_MASK                0x000001FFUL
#define SETTINGS_REG_INFO_DIAGFAULT_COUNT_OFFS              16
#define SETTINGS_REG_INFO_DIAGFAULT_COUNT_MASK              0xFFFF0000UL
/* 0xE3 to 0xEA: the crash record, zero if none: stacked PC, LR and xPSR, then SCB CFSR, HFSR,
 * MMFAR, BFAR and the EXC_RETURN value */
#define SETTINGS_REG_INFO_DIAGPC                            0xE3
#define SETTINGS_REG_INFO_DIAGLR                            0xE4
#define SETTINGS_REG_INFO_DIAGXPSR                          0xE5
#define SETTINGS_REG_INFO_DIAGCFSR                          0xE6
#define SETTINGS_REG_INFO_DIAGHFSR                          0xE7
#define SETTINGS_REG_INFO_DIAGMMFAR                         0xE8
#define SETTINGS_REG_INFO_DIAGBFAR                          0xE9
#define SETTINGS_REG_INFO_DIAGEXCRET                        0xEA
/* 0xEB DIAGUPTIME: ms from boot to the last 1 ms tick before the reset */
#define SETTINGS_REG_INFO_DIAGUPTIME                        0xEB
/* 0xEC DIAGAGE0 and 0xED DIAGAGE1: for the main loop (0xEC bits 0-15), the USB interrupt
 * (0xEC bits 16-31), the recording interrupt (0xED bits 0-15) and the playback interrupt (0xED
 * bits 16-31; both the DMA block interrupts), how many ms before that last tick each last ran; 0xFFFF if not since boot (or that
 * long or longer) */
#define SETTINGS_REG_INFO_DIAGAGE0                          0xEC
#define SETTINGS_REG_INFO_DIAGAGE1                          0xED
/* 0xEE DIAGSTACK: bytes at the bottom of the stack the last run never used (bits 0-15; 0
 * means it reached the bottom and probably overflowed into .bss; 0xFFFF unknown, after a
 * power-on) and the stack size in bytes (bits 16-31) */
#define SETTINGS_REG_INFO_DIAGSTACK                         0xEE
/* 0xEF DIAGLOOPS (live, not from the last run): main-loop passes in the last whole second,
 * updated every second. Each pass refreshes the watchdog; if the interrupts left the main
 * loop no time, this would fall towards 0 before a watchdog reset */
#define SETTINGS_REG_INFO_DIAGLOOPS                         0xEF

/* UAC audio debug register 0 */
#define SETTINGS_REG_INFO_AUDIO0                            0xD0
#define SETTINGS_REG_INFO_AUDIO0_DEFAULT                    0
/* Playback or recording muted (master and first channel) */
#define SETTINGS_REG_INFO_AUDIO0_RECMUTE0_MASK              0x00010000UL
#define SETTINGS_REG_INFO_AUDIO0_RECMUTE1_MASK              0x00020000UL
#define SETTINGS_REG_INFO_AUDIO0_PLAYMUTE0_MASK             0x00100000UL
#define SETTINGS_REG_INFO_AUDIO0_PLAYMUTE1_MASK             0x00200000UL
/* Playback and recording state */
#define SETTINGS_REG_INFO_AUDIO0_RECSTATE_OFFS              24
#define SETTINGS_REG_INFO_AUDIO0_RECSTATE_MASK              0x0F000000UL
#define SETTINGS_REG_INFO_AUDIO0_RECSTATE_OFF_ENUM          0
#define SETTINGS_REG_INFO_AUDIO0_RECSTATE_START_ENUM        1
#define SETTINGS_REG_INFO_AUDIO0_RECSTATE_RUN_ENUM          2
#define SETTINGS_REG_INFO_AUDIO0_PLAYSTATE_OFFS             28
#define SETTINGS_REG_INFO_AUDIO0_PLAYSTATE_MASK             0xF0000000UL
#define SETTINGS_REG_INFO_AUDIO0_PLAYSTATE_OFF_ENUM         0
#define SETTINGS_REG_INFO_AUDIO0_PLAYSTATE_START_ENUM       1
#define SETTINGS_REG_INFO_AUDIO0_PLAYSTATE_RUN_ENUM         2

/* Audio debug register 1 */
#define SETTINGS_REG_INFO_AUDIO1                            0xD1
#define SETTINGS_REG_INFO_AUDIO1_DEFAULT                    0

/* Audio debug register 2 */
#define SETTINGS_REG_INFO_AUDIO2                            0xD2
#define SETTINGS_REG_INFO_AUDIO2_DEFAULT                    0
/* Recording samplerate */
#define SETTINGS_REG_INFO_AUDIO2_RECRATE_OFFS               0
#define SETTINGS_REG_INFO_AUDIO2_RECRATE_MASK               0xFFFFFFFFUL

/* Audio debug register 3 */
#define SETTINGS_REG_INFO_AUDIO3                            0xD3
#define SETTINGS_REG_INFO_AUDIO3_DEFAULT                    0
/* Recording volume (master and first channel) */
#define SETTINGS_REG_INFO_AUDIO3_RECVOL0_OFFS               0
#define SETTINGS_REG_INFO_AUDIO3_RECVOL0_MASK               0x0000FFFFUL
#define SETTINGS_REG_INFO_AUDIO3_RECVOL1_OFFS               16
#define SETTINGS_REG_INFO_AUDIO3_RECVOL1_MASK               0xFFFF0000UL

/* TODO: D4, D5, D6 -> Recording buffer levels (avg/min/max) */

/* Audio debug register 4 */
#define SETTINGS_REG_INFO_AUDIO4                            0xD4
#define SETTINGS_REG_INFO_AUDIO4_DEFAULT                    0

/* Audio debug register 5 */
#define SETTINGS_REG_INFO_AUDIO5                            0xD5
#define SETTINGS_REG_INFO_AUDIO5_DEFAULT                    0

/* Audio debug register 6 */
#define SETTINGS_REG_INFO_AUDIO6                            0xD6
#define SETTINGS_REG_INFO_AUDIO6_DEFAULT                    0

/* Audio debug register 7 */
#define SETTINGS_REG_INFO_AUDIO7                            0xD7
#define SETTINGS_REG_INFO_AUDIO7_DEFAULT                    0

/* Audio debug register 8 */
#define SETTINGS_REG_INFO_AUDIO8                            0xD8
#define SETTINGS_REG_INFO_AUDIO8_DEFAULT                    0
/* Playback samplerate */
#define SETTINGS_REG_INFO_AUDIO8_PLAYRATE_OFFS              0
#define SETTINGS_REG_INFO_AUDIO8_PLAYRATE_MASK              0xFFFFFFFFUL

/* Audio debug register 9 */
#define SETTINGS_REG_INFO_AUDIO9                            0xD9
#define SETTINGS_REG_INFO_AUDIO9_DEFAULT                    0
/* Playback volume (master and first channel) */
#define SETTINGS_REG_INFO_AUDIO9_PLAYVOL0_OFFS              0
#define SETTINGS_REG_INFO_AUDIO9_PLAYVOL0_MASK              0x0000FFFFUL
#define SETTINGS_REG_INFO_AUDIO9_PLAYVOL1_OFFS              16
#define SETTINGS_REG_INFO_AUDIO9_PLAYVOL1_MASK              0xFFFF0000UL

/* Audio debug register 10 */
#define SETTINGS_REG_INFO_AUDIO10                           0xDA
#define SETTINGS_REG_INFO_AUDIO10_DEFAULT                   0
/* Average playback buffer level, in bytes: the USB FIFO plus what the DMA buffer still holds to
 * play. The feedback endpoint holds it at 5 frames plus one block (576 at 48 kHz) */
#define SETTINGS_REG_INFO_AUDIO10_PLAYBUFAVG_OFFS           0
#define SETTINGS_REG_INFO_AUDIO10_PLAYBUFAVG_MASK           0x0000FFFFUL

/* Audio debug register 11 */
#define SETTINGS_REG_INFO_AUDIO11                           0xDB
#define SETTINGS_REG_INFO_AUDIO11_DEFAULT                   0
/* Minimum playback buffer level */
#define SETTINGS_REG_INFO_AUDIO11_PLAYBUFMIN_OFFS           0
#define SETTINGS_REG_INFO_AUDIO11_PLAYBUFMIN_MASK           0x0000FFFFUL

/* Audio debug register 12 */
#define SETTINGS_REG_INFO_AUDIO12                           0xDC
#define SETTINGS_REG_INFO_AUDIO12_DEFAULT                   0
/* Maximum playback buffer level */
#define SETTINGS_REG_INFO_AUDIO12_PLAYBUFMAX_OFFS           0
#define SETTINGS_REG_INFO_AUDIO12_PLAYBUFMAX_MASK           0x0000FFFFUL

/* Audio debug register 13 */
#define SETTINGS_REG_INFO_AUDIO13                           0xDD
#define SETTINGS_REG_INFO_AUDIO13_DEFAULT                   0
/* Average UAC2.0 Feedback Value since last playback */
#define SETTINGS_REG_INFO_AUDIO13_PLAYFBAVG_OFFS            0
#define SETTINGS_REG_INFO_AUDIO13_PLAYFBAVG_MASK            0xFFFFFFFFUL

/* Audio debug register 14 */
#define SETTINGS_REG_INFO_AUDIO14                           0xDE
#define SETTINGS_REG_INFO_AUDIO14_DEFAULT                   0
/* Minimum UAC2.0 Feedback Value since last playback */
#define SETTINGS_REG_INFO_AUDIO14_PLAYFBMIN_OFFS            0
#define SETTINGS_REG_INFO_AUDIO14_PLAYFBMIN_MASK            0xFFFFFFFFUL

/* Audio debug register 15 */
#define SETTINGS_REG_INFO_AUDIO15                           0xDF
#define SETTINGS_REG_INFO_AUDIO15_DEFAULT                   0
/* Maximum UAC2.0 Feedback Value since last playback */
#define SETTINGS_REG_INFO_AUDIO15_PLAYFBMAX_OFFS            0
#define SETTINGS_REG_INFO_AUDIO15_PLAYFBMAX_MASK            0xFFFFFFFFUL


void Settings_Init();
uint8_t Settings_RegWrite(uint8_t address, uint32_t data);
uint8_t Settings_RegRead(uint8_t address, uint32_t * data);
void Settings_Store(void);
void Settings_Recall(void);
void Settings_Default(void);

#endif /* SETTINGS_H_ */
