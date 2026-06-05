#include <avr/io.h>
#include "bl_config.h"

/*
 * Fuses written to the device when the bootloader hex is flashed via UPDI.
 * These are programmed once at the factory and never touched by the
 * bootloader at runtime — the bootloader only writes APPCODE flash, never
 * fuses, so an in-field application update cannot change them.
 *
 * BOOTSIZE/CODESIZE/WDTCFG are bootloader-critical and have fixed defaults
 * that match the linker script. Everything else has a product-overridable
 * default so the bootloader-flashed device matches the application's
 * expectations for BOD level, oscillator frequency, etc.
 */

#ifndef BL_FUSE_BODCFG
#  define BL_FUSE_BODCFG  (ACTIVE_DISABLE_gc | LVL_BODLEVEL0_gc | \
                           SAMPFREQ_128HZ_gc  | SLEEP_DISABLE_gc)
#endif
#ifndef BL_FUSE_OSCCFG
#  define BL_FUSE_OSCCFG  OSCHFFRQ_20M_gc
#endif
#ifndef BL_FUSE_SYSCFG0
#  define BL_FUSE_SYSCFG0 (CRCSEL_CRC16_gc | CRCSRC_NOCRC_gc | \
                           RSTPINCFG_NONE_gc | UPDIPINCFG_UPDI_gc)
#endif
#ifndef BL_FUSE_SYSCFG1
#  define BL_FUSE_SYSCFG1 SUT_64MS_gc
#endif
#ifndef BL_FUSE_BOOTSIZE
#  define BL_FUSE_BOOTSIZE 0x10   /* 4 KB BOOT = 16 × 256B; must match Makefile BOOT_SIZE */
#endif
#ifndef BL_FUSE_CODESIZE
#  define BL_FUSE_CODESIZE 0x00   /* APPCODE fills flash from BOOTEND+1 to FLASHEND */
#endif
#ifndef BL_FUSE_WDTCFG
#  define BL_FUSE_WDTCFG   0x00   /* WDT disabled at reset; app may enable at runtime */
#endif

FUSES = {
    .BODCFG   = BL_FUSE_BODCFG,
    .BOOTSIZE = BL_FUSE_BOOTSIZE,
    .CODESIZE = BL_FUSE_CODESIZE,
    .OSCCFG   = BL_FUSE_OSCCFG,
    .SYSCFG0  = BL_FUSE_SYSCFG0,
    .SYSCFG1  = BL_FUSE_SYSCFG1,
    .WDTCFG   = BL_FUSE_WDTCFG,
};
