# ===========================================================================
#  bldc_control_final_v6 (board v7)  --  STM32G431CBT6 + DRV8316T
#
#  One target per application. Every app links the same common/ + HAL base
#  and produces its own .elf/.bin/.hex, so it can be flashed on its own.
#
#  Examples:
#      make APP=test_00_hello          build one app (default APP=foc)
#      make APP=test_07_esp_link       build ESP link test
#      make APP=foc_esp flash          build and flash FOC + ESP app
#                                      (USB-C DFU if the board is seen on
#                                      USB, otherwise ST-Link/SWD)
#      make APP=foc flash-dfu          force USB DFU
#      make APP=foc flash-swd          force ST-Link/SWD (OpenOCD)
#      make apps                       build all apps
#      make APP=foc size               show flash/RAM usage
#      make clean
#
#  Toolchain: arm-none-eabi-gcc (apt install gcc-arm-none-eabi).
#  Flashing:  dfu-util over USB-C (default), or openocd + ST-Link.
# ===========================================================================

CROSS   ?= arm-none-eabi-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
SIZE    := $(CROSS)size

# ---- application selection ------------------------------------------------
APP ?= foc
ENCODER_MODE ?= i2c

ifeq ($(ENCODER_MODE),adc)
ENCODER_SOURCE_ADC := 1
else ifeq ($(ENCODER_MODE),i2c)
ENCODER_SOURCE_ADC := 0
else
$(error Unknown ENCODER_MODE '$(ENCODER_MODE)'. Use i2c or adc)
endif

APPS := test_00_hello \
        test_01_as5600 \
        test_02_drv8316 \
        test_03_pwm \
        test_04_current_sense \
        test_05_open_loop \
        test_06_encoder_cal \
        test_07_esp_link \
        test_08_drive_lab \
        test_09_drv_diag \
        foc \
        foc_esp

ifeq ($(filter $(APP),$(APPS)),)
$(error Unknown APP '$(APP)'. Valid apps: $(APPS))
endif

ifeq ($(APP),foc)
APP_DIR   := foc
APP_EXTRA := foc/foc.c foc/encoder.c foc/as5600.c
else ifeq ($(APP),foc_esp)
APP_DIR   := foc_esp
APP_EXTRA := foc/foc.c foc/encoder.c foc/as5600.c foc_esp/esp_link.c
else
APP_DIR   := tests/$(APP)
# Modules a given test needs from foc/ and foc_esp/
ifeq ($(APP),test_01_as5600)
APP_EXTRA := foc/as5600.c
endif
ifeq ($(APP),test_06_encoder_cal)
APP_EXTRA := foc/as5600.c foc/encoder.c
endif
ifeq ($(APP),test_07_esp_link)
APP_EXTRA := foc_esp/esp_link.c
endif
endif

APP_EXTRA ?=

# ---- paths ----------------------------------------------------------------
BUILD    := build
OBJDIR   := $(BUILD)/$(APP)/obj/$(ENCODER_MODE)
TARGET   := $(BUILD)/$(APP)/$(APP)
LDSCRIPT := common/STM32G431CBTX_FLASH.ld

# ---- sources --------------------------------------------------------------
COMMON_SRCS := \
    common/startup_stm32g431cbtx.s \
    common/system_stm32g4xx.c \
    common/sysmem.c \
    common/board.c \
    common/usb_console.c \
    common/usb_descriptors.c \
    common/stm32g4xx_hal_msp.c \
    common/stm32g4xx_it.c

# TinyUSB 0.17.0 (vendored, MIT): USB CDC console on the USB-C connector.
TUSB := Middlewares/tinyusb/src
TUSB_SRCS := \
    $(TUSB)/tusb.c \
    $(TUSB)/common/tusb_fifo.c \
    $(TUSB)/device/usbd.c \
    $(TUSB)/device/usbd_control.c \
    $(TUSB)/class/cdc/cdc_device.c \
    $(TUSB)/portable/st/stm32_fsdev/dcd_stm32_fsdev.c

HAL_SRCS := \
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal.c \
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_adc.c \
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_adc_ex.c \
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_cortex.c \
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_dma.c \
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_dma_ex.c \
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_exti.c \
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_flash.c \
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_flash_ex.c \
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_flash_ramfunc.c \
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_gpio.c \
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_i2c.c \
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_i2c_ex.c \
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_pwr.c \
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_pwr_ex.c \
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_rcc.c \
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_rcc_ex.c \
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_tim.c \
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_tim_ex.c \
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_uart.c \
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_uart_ex.c

APP_SRCS := $(APP_DIR)/main.c $(APP_EXTRA)

SRCS := $(COMMON_SRCS) $(TUSB_SRCS) $(HAL_SRCS) $(APP_SRCS)
OBJS := $(addprefix $(OBJDIR)/,$(addsuffix .o,$(SRCS)))
DEPS := $(OBJS:.o=.d)

# ---- flags ----------------------------------------------------------------
MCU     := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
# USER_DEFS passes extra -D options, e.g. USER_DEFS="-DFOC_VBUS=9.0f".
USER_DEFS ?=
# USER_VECT_TAB_ADDRESS: SystemInit() sets VTOR = 0x08000000. Required: after
# "dfu-util ... :leave" the ROM bootloader jumps into the app without a reset,
# so VTOR would still point at the bootloader's vector table (0x1FFF0000).
# The verified bring-up firmware was built with this flag too.
DEFS    := -DSTM32G431xx -DUSE_HAL_DRIVER -DUSER_VECT_TAB_ADDRESS \
           -DENCODER_SOURCE_ADC=$(ENCODER_SOURCE_ADC) \
           -DAPP_NAME=\"$(APP)\" $(USER_DEFS)
INCS    := -Icommon -Ifoc -Ifoc_esp -I$(APP_DIR) -I$(TUSB) \
           -IDrivers/STM32G4xx_HAL_Driver/Inc \
           -IDrivers/STM32G4xx_HAL_Driver/Inc/Legacy \
           -IDrivers/CMSIS/Device/ST/STM32G4xx/Include \
           -IDrivers/CMSIS/Include

CFLAGS  := $(MCU) $(DEFS) $(INCS) -O2 -g3 \
           -Wall -Wextra -Wno-unused-parameter \
           -ffunction-sections -fdata-sections -fno-common -MMD -MP
ASFLAGS := $(MCU) $(DEFS) $(INCS) -g3
LDFLAGS := $(MCU) --specs=nano.specs --specs=nosys.specs \
           -Wl,--gc-sections -Wl,--print-memory-usage -Wl,--no-warn-rwx-segments \
           -Wl,-u,_printf_float -lm

# ---- flashing -------------------------------------------------------------
# tools/flash.py picks USB-C DFU when the board is on USB (running app or ROM
# DFU), otherwise falls back to OpenOCD + ST-Link on J5.
PYTHON      ?= python3
FLASH_MODE  ?= auto
OPENOCD     ?= openocd
OOCD_INTERFACE ?= interface/stlink.cfg
OOCD_TARGET    ?= target/stm32g4x.cfg

# ===========================================================================
.PHONY: all apps flash flash-dfu flash-swd size clean help list

all: $(TARGET).elf $(TARGET).bin $(TARGET).hex

$(TARGET).elf: $(OBJS) $(LDSCRIPT) Makefile
	@mkdir -p $(dir $@)
	$(CC) $(OBJS) $(LDFLAGS) -T$(LDSCRIPT) -Wl,-Map=$(TARGET).map -o $@
	@$(SIZE) $@

$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@

$(TARGET).hex: $(TARGET).elf
	$(OBJCOPY) -O ihex $< $@

$(OBJDIR)/%.c.o: %.c Makefile
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS) $< -o $@

$(OBJDIR)/%.s.o: %.s Makefile
	@mkdir -p $(dir $@)
	$(CC) -x assembler-with-cpp -c $(ASFLAGS) $< -o $@

apps:
	@for a in $(APPS); do \
	    $(MAKE) --no-print-directory APP=$$a all || exit 1; \
	done

flash: $(TARGET).elf $(TARGET).bin
	$(PYTHON) tools/flash.py --mode $(FLASH_MODE) --bin $(TARGET).bin --elf $(TARGET).elf \
	    --openocd "$(OPENOCD)" --interface $(OOCD_INTERFACE) --target $(OOCD_TARGET)

flash-dfu:
	@$(MAKE) --no-print-directory APP=$(APP) FLASH_MODE=dfu flash

flash-swd:
	@$(MAKE) --no-print-directory APP=$(APP) FLASH_MODE=swd flash

size: $(TARGET).elf
	@$(SIZE) $(TARGET).elf

list:
	@echo "APPS: $(APPS)"
	@echo "current APP: $(APP)  ->  $(TARGET).elf"

clean:
	rm -rf $(BUILD)

help:
	@echo "bldc_control_final_v6 (board v7) firmware"
	@echo
	@echo "  make APP=<app>          build one app (default: foc)"
	@echo "  make APP=<app> flash    build + flash (USB-C DFU if seen, else ST-Link)"
	@echo "  make APP=<app> flash-dfu / flash-swd   force one method"
	@echo "  make apps               build every app"
	@echo "  make APP=<app> size     show flash/RAM usage"
	@echo "  make list               list app names"
	@echo "  make clean              remove build/"
	@echo
	@echo "  apps: $(APPS)"

-include $(DEPS)
