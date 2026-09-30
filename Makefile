# Build the AIOC firmware with plain arm-none-eabi-gcc, no STM32CubeIDE needed.
#
# Upstream (skuep/AIOC) builds with the STM32CubeIDE project in stm32/aioc-fw
# (.project, .cproject). The settings below (MCU, defines, include paths, -O3)
# are the Release configuration of that project, copied by hand.
#
#   make                      build/aioc-fw.{elf,bin,hex} and build/aioc-fw-keep-settings.bin,
#                             with the TX equaliser defaulting to the "K5 on red v1 AIOC"
#                             profile (k5-red)
#   make TXEQ_DEFAULT=off     same, but the equaliser defaults to off, for development
#                             (use BUILD=... for a separate output directory)
#   make test                 host unit tests for the equaliser and the settings page (bench/)
#
# The CI (.github/workflows) pins the toolchain version; see README.md.

AIOC_SRC ?= stm32/aioc-fw
BUILD    ?= build
TXEQ_DEFAULT ?= k5-red

CROSS   ?= arm-none-eabi-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
SIZE    := $(CROSS)size

MCU_FLAGS := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard

DEFINES := -DSTM32F302xC -DUSER_VECT_TAB_ADDRESS -DCFG_TUSB_MCU=OPT_MCU_STM32F3

ifeq ($(TXEQ_DEFAULT),off)
DEFINES += -DTXEQ_DEFAULT_OFF
else ifneq ($(TXEQ_DEFAULT),k5-red)
$(error TXEQ_DEFAULT must be k5-red or off)
endif

INCLUDES := \
  -I$(AIOC_SRC)/Inc \
  -I$(AIOC_SRC)/Src \
  -I$(AIOC_SRC)/Drivers/CMSIS/Include \
  -I$(AIOC_SRC)/Drivers/CMSIS/Device/ST/STM32F3xx/Include \
  -I$(AIOC_SRC)/Drivers/STM32F3xx_HAL_Driver/Inc \
  -I$(AIOC_SRC)/Middlewares/Third-Party/tinyusb/src

# Application sources; the startup file is assembled separately below.
APP_SRCS := $(filter-out %/startup_stm32f302xc.s,$(wildcard $(AIOC_SRC)/Src/*.c))
STARTUP  := $(AIOC_SRC)/Src/startup_stm32f302xc.s

# Every HAL driver source except the _template files, as the IDE does. Modules
# that stm32f3xx_hal_conf.h does not enable compile to empty objects. The
# templates would define weak callbacks the application already provides.
HAL_SRCS := $(filter-out %_template.c,$(wildcard $(AIOC_SRC)/Drivers/STM32F3xx_HAL_Driver/Src/*.c))

# tinyusb: the device stack, the classes tusb_config.h enables (audio, CDC,
# HID, DFU runtime) and the STM32 fsdev device controller driver.
TUSB_DIR := $(AIOC_SRC)/Middlewares/Third-Party/tinyusb/src
TUSB_SRCS := \
  $(TUSB_DIR)/tusb.c \
  $(TUSB_DIR)/common/tusb_fifo.c \
  $(TUSB_DIR)/device/usbd.c \
  $(TUSB_DIR)/device/usbd_control.c \
  $(TUSB_DIR)/class/audio/audio_device.c \
  $(TUSB_DIR)/class/cdc/cdc_device.c \
  $(TUSB_DIR)/class/hid/hid_device.c \
  $(TUSB_DIR)/class/dfu/dfu_rt_device.c \
  $(TUSB_DIR)/portable/st/stm32_fsdev/dcd_stm32_fsdev.c

SRCS := $(APP_SRCS) $(HAL_SRCS) $(TUSB_SRCS)
OBJS := $(patsubst %.c,$(BUILD)/%.o,$(notdir $(SRCS)))
OBJS += $(BUILD)/startup_stm32f302xc.o

vpath %.c $(sort $(dir $(SRCS)))

CFLAGS := $(MCU_FLAGS) $(DEFINES) $(INCLUDES) -O3 -ffunction-sections -fdata-sections \
          -Wall -std=gnu11 -g3 -MMD -MP

LDFLAGS := $(MCU_FLAGS) -specs=nano.specs -specs=nosys.specs \
           -T$(AIOC_SRC)/stm32f30_flash.ld -Wl,--gc-sections -Wl,-Map=$(BUILD)/aioc-fw.map

TARGET := $(BUILD)/aioc-fw

.PHONY: all clean size test check-submodule
all: check-submodule $(TARGET).elf $(TARGET).bin $(TARGET)-keep-settings.bin $(TARGET).hex size

check-submodule:
	@test -e $(TUSB_DIR)/tusb.c || { echo "tinyusb is missing: run git submodule update --init" >&2; exit 1; }

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/startup_stm32f302xc.o: $(STARTUP) | $(BUILD)
	$(CC) $(MCU_FLAGS) -x assembler-with-cpp -c $< -o $@

$(TARGET).elf: $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -Wl,--start-group -lc -lm -lnosys -Wl,--end-group -o $@

# The full image: everything up to and including the settings page at
# 0x0801F000, which upstream deliberately fills with 0xFF so that flashing it
# resets the stored settings to the firmware defaults.
$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@

# The same image without the settings page, so flashing it leaves the stored
# settings as they are (DFU only erases the pages it writes).
$(TARGET)-keep-settings.bin: $(TARGET).elf
	$(OBJCOPY) -O binary -R .eeprom $< $@

$(TARGET).hex: $(TARGET).elf
	$(OBJCOPY) -O ihex $< $@

size: $(TARGET).elf
	$(SIZE) $<

test:
	$(MAKE) -C bench test

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)
