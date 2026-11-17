CROSS  ?= arm-none-eabi-
CC      = $(CROSS)gcc
OBJCOPY = $(CROSS)objcopy
SIZE    = $(CROSS)size

# Libraries (TinyUSB, CMSIS) are shared with the hello-world project.
LIB  ?= $(HOME)/stm32g431-hello/lib
TUSB = $(LIB)/tinyusb/src
DEV  = $(LIB)/cmsis_device_g4

SRCS = src/main.c src/usb_descriptors.c \
       $(DEV)/Source/Templates/system_stm32g4xx.c \
       $(TUSB)/tusb.c $(TUSB)/common/tusb_fifo.c \
       $(TUSB)/device/usbd.c $(TUSB)/device/usbd_control.c \
       $(TUSB)/class/cdc/cdc_device.c \
       $(TUSB)/portable/st/stm32_fsdev/dcd_stm32_fsdev.c
ASRCS = $(DEV)/Source/Templates/gcc/startup_stm32g431xx.s

AUTOSTART ?= 0
DRIVE     ?= sixstep
DRIVE_SINE = $(if $(filter sine,$(DRIVE)),1,0)

MCU    = -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16
CFLAGS = $(MCU) -Os -g3 -Wall -ffunction-sections -fdata-sections \
         -DSTM32G431xx -DUSER_VECT_TAB_ADDRESS -DAUTOSTART=$(AUTOSTART) -DDRIVE_SINE=$(DRIVE_SINE) \
         -Isrc -I$(TUSB) -I$(DEV)/Include -I$(LIB)/cmsis5/CMSIS/Core/Include
LDFLAGS = $(MCU) -T STM32G431CB.ld -Wl,--gc-sections -specs=nano.specs -specs=nosys.specs \
          -Wl,-Map=build/fw.map -Wl,--print-memory-usage -Wl,--no-warn-rwx-segments -lm

# Library objects go under build/lib/ regardless of where LIB lives.
obj = build/$(patsubst $(LIB)/%,lib/%,$(1))
OBJS = $(foreach s,$(SRCS),$(call obj,$(s:.c=.o))) $(foreach s,$(ASRCS),$(call obj,$(s:.s=.o)))

all: build/fw.bin

build/src/%.o: src/%.c src/tusb_config.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

build/lib/%.o: $(LIB)/%.c src/tusb_config.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

build/lib/%.o: $(LIB)/%.s
	@mkdir -p $(dir $@)
	$(CC) $(MCU) -c $< -o $@

build/fw.elf: $(OBJS) STM32G431CB.ld
	$(CC) $(OBJS) $(LDFLAGS) -o $@
	$(SIZE) $@

build/fw.bin: build/fw.elf
	$(OBJCOPY) -O binary $< $@

# If an app (this one or the hello app) is running, send it "dfu" to reboot
# into the ST bootloader; otherwise the board must already be in DFU mode.
APP_TTY = $(firstword $(wildcard /dev/serial/by-id/usb-Custom_STM32G431_*-if00))

flash: build/fw.bin
	@if [ -n "$(APP_TTY)" ] && ! lsusb -d 0483:df11 >/dev/null 2>&1; then \
	  echo "App running: sending 'dfu' command"; printf 'dfu\r' > "$(APP_TTY)"; \
	fi
	@for i in $$(seq 1 20); do lsusb -d 0483:df11 >/dev/null 2>&1 && break; sleep 0.5; done; \
	 lsusb -d 0483:df11 >/dev/null 2>&1 || { echo "No DFU device found. Put BOOT0 high and replug."; exit 1; }
	@sleep 0.5
	@dfu-util -a 0 -s 0x08000000:leave -D build/fw.bin 2>&1 | tee build/dfu.log | grep -E "^(Download|Erase|File|dfu-util:)" ; \
	 grep -q "File downloaded successfully" build/dfu.log && echo "Flash OK (a 'get_status' error after leave is normal)"

clean:
	rm -rf build

.PHONY: all flash clean
