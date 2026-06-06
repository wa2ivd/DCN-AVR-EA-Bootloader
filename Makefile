# DCN-AVR-EA-Bootloader Makefile
#
# Usage:
#   make PRODUCT=SPS-1                build bootloader hex + lss + size report
#   make PRODUCT=SPS-1 clean          remove build artefacts
#   make PRODUCT=SPS-1 fuses          emit just the fuse hex for UPDI flashing
#   make PRODUCT=SPS-1 BOOT_SIZE=4096 override BOOT section size limit
#
# Requires:
#   avr-gcc, avr-objcopy, avr-objdump, avr-size (avr-gcc toolchain >= 12.x
#   recommended for full AVR EA device-pack support, but XC8 for AVR also
#   works since it's avr-gcc underneath).
#   Microchip AVR EA device pack must be installed and findable by the
#   compiler (the XC8 install path or the official avr-libc/atpacks layout).

# ---- Per-product knobs ----
PRODUCT     ?= SPS-1
MCU         ?= avr64ea28
F_CPU       ?= 10000000UL
# Must match (BL_FUSE_BOOTSIZE * 256). 0x10 * 256 = 4096.
BOOT_SIZE   ?= 4096

# Path to a Microchip device pack (DFP). Required for AVR EA support with
# avr-gcc / XC8 for AVR — the DFP supplies specs-avr64ea28, the device
# header (ioavr64ea28.h), the runtime (crtavr64ea28.o), and the device lib.
# Override on the command line if your DFP lives elsewhere, e.g.:
#   make PRODUCT=SPS-1 DFP="/c/Program Files/Microchip/MPLABX/v6.25/packs/Microchip/AVR-Ex_DFP/2.11.221"
DFP         ?=

# ---- Layout ----
SRC_DIR     := src
PROD_DIR    := products/$(PRODUCT)
BUILD_DIR   := build/$(PRODUCT)

# ---- Toolchain ----
CC          := avr-gcc
OBJCOPY     := avr-objcopy
OBJDUMP     := avr-objdump
SIZE        := avr-size

# ---- Sources ----
SRCS        := $(wildcard $(SRC_DIR)/*.c)
OBJS        := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))
DEPS        := $(OBJS:.o=.d)

# ---- Flags ----
WARNFLAGS   := -Wall -Wextra -Wstrict-prototypes -Wundef -Wshadow \
               -Wno-unused-parameter

# Add DFP search paths if a DFP was provided. avr-gcc's -B flag tells the
# driver to look in the given directory for both device-specs and the
# device runtime (crt*.o, lib*.a).
DFP_FLAGS   :=
ifneq ($(strip $(DFP)),)
  DFP_FLAGS += -B "$(DFP)/gcc/dev/$(MCU)"
  DFP_FLAGS += -isystem "$(DFP)/include"
endif

CFLAGS      := -mmcu=$(MCU) -DF_CPU=$(F_CPU) $(DFP_FLAGS) \
               -I$(SRC_DIR) -I$(PROD_DIR) \
               -Os -ffunction-sections -fdata-sections \
               -fshort-enums -funsigned-char -funsigned-bitfields \
               -mrelax -std=gnu11 \
               $(WARNFLAGS) -MMD -MP

             # avr-gcc's default linker script already places .text at 0x0000,
             # which is exactly where the BOOT section starts. Explicitly
             # passing --section-start=.text=0x0000 conflicts with the default
             # layout, so we just let the default placement apply here. The
             # *application* build is the one that needs --section-start to
             # move .text up to BL_APP_START (see app_support/README.md).
LDFLAGS     := -mmcu=$(MCU) -mrelax $(DFP_FLAGS) \
               -Wl,--gc-sections \
               -Wl,-Map=$(BUILD_DIR)/$(PRODUCT)-bootloader.map

TARGET      := $(BUILD_DIR)/$(PRODUCT)-bootloader

# ---- Rules ----
.PHONY: all clean size hex lss help

all: hex size lss

help:
	@echo "DCN-AVR-EA-Bootloader build targets:"
	@echo "  make PRODUCT=<name>          full build (hex + size + lss)"
	@echo "  make PRODUCT=<name> hex      just produce the hex"
	@echo "  make PRODUCT=<name> size     show size and enforce BOOT limit"
	@echo "  make PRODUCT=<name> clean    remove build/$(PRODUCT)"
	@echo ""
	@echo "Current settings:"
	@echo "  PRODUCT=$(PRODUCT)  MCU=$(MCU)  F_CPU=$(F_CPU)  BOOT_SIZE=$(BOOT_SIZE)"

$(BUILD_DIR):
	@mkdir -p $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(TARGET).elf: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

# Combined bootloader hex — flash + fuses. .fuse is kept in (the FUSES
# block in bl_fuses.c lands at the AVR fuse LMA 0x820000) so MPLAB IPE
# and equivalent tools see fuse bytes alongside flash, exactly like the
# application's MPLAB-produced hex. .eeprom / .lock / .signature /
# .user_signatures are stripped because they're either empty here or
# unwanted in a flash-programming artefact.
$(TARGET).hex: $(TARGET).elf
	$(OBJCOPY) -O ihex -R .eeprom -R .lock -R .signature -R .user_signatures $< $@

$(TARGET).lss: $(TARGET).elf
	$(OBJDUMP) -h -S $< > $@

hex:   $(TARGET).hex
lss:   $(TARGET).lss

size: $(TARGET).elf
	@$(SIZE) -C --mcu=$(MCU) $< || $(SIZE) -A $<
	@# Sum every section that ends up in flash: .text* (split by -ffunction-sections),
	@# .rodata* (PROGMEM string literals and lookup tables), and .data (initializers
	@# get a copy in flash even though they live in SRAM at runtime). Counting only
	@# .text* previously missed ~600 B of rodata strings and let a near-overflow slip
	@# silently right up to the BOOT/APPCODE boundary.
	@flash_size=$$( $(SIZE) -A $< | awk '/^\.(text|rodata|data)/{s+=$$2} END{print s+0}' ); \
	echo "Total flash footprint: $$flash_size  /  BOOT limit: $(BOOT_SIZE) bytes"; \
	if [ $$flash_size -gt $(BOOT_SIZE) ]; then \
	    echo "ERROR: bootloader flash footprint ($$flash_size B) overflows BOOT section ($(BOOT_SIZE) B)."; \
	    echo "       Either trim it or grow BL_FUSE_BOOTSIZE (and BOOT_SIZE) together."; \
	    exit 1; \
	fi

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPS)
