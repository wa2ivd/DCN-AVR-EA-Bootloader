#ifndef BL_PRODUCT_CONFIG_H
#define BL_PRODUCT_CONFIG_H

/*
 * Template product config. Copy this directory to products/<NewProduct>/
 * and fill in the constants below. The Makefile picks the product config
 * up via `make PRODUCT=<NewProduct>`.
 *
 * Required entries are flagged "REQUIRED" — without them the build will
 * fail at the #error checks in bl_config.h.
 */

#include <avr/io.h>

#define BL_PRODUCT_NAME    "TemplateProduct"     /* REQUIRED */

/* Address the device responds to while in bootloader mode. Pick a value
 * that does not collide with any normal device address used on the bus. */
#define BL_DCN_ADDRESS     0xBBu                 /* REQUIRED */

/* UART selection — set BL_USART to the device's USART instance (USART0,
 * USART1, ...) and the bus/wiring-specific CTRLA / CTRLB values. CTRLC
 * is hardcoded to 8N1 inside bl_uart.c. Default omits both registers,
 * giving you plain push-pull TX and no RS-485 mode.
 *
 * RS-485 gotcha: if you set the RS485 field in CTRLA to "EXT" (mode 01)
 * so the USART drives an external transceiver's DE/~RE, the XDIRn pin's
 * alternate-function override only kicks in when that pin's PORTx.DIR
 * bit is set to output. BL_PORT_INIT must include it explicitly — the
 * USART won't set DIR for you, and without it your bootloader TX bytes
 * are written to TXDATAL but never make it onto the differential bus. */
#define BL_USART           USART0
#define BL_BAUD            9600UL                /* REQUIRED */
/* #define BL_USART_CTRLA  0x00 */
/* #define BL_USART_CTRLB  (USART_RXEN_bm | USART_TXEN_bm) */

/* Memory layout — must agree with bl_fuses.c BL_FUSE_BOOTSIZE.
 *   Default BOOTSIZE = 0x10 → BOOT is 0x0000–0x0FFF (4 KB).
 *   APPCODE starts immediately after, at BL_APP_START. */
#define BL_APP_START       0x1000u               /* REQUIRED */
#define BL_FLASH_END       0xFFFFu               /* REQUIRED */
#define BL_PAGE_SIZE       128u                  /* REQUIRED */
#define BL_APP_CRC_ADDR    0xFFFEu               /* REQUIRED — top of APPCODE, 2-byte trailer */

/* Version reported by BLINFO. Bump on each released change. */
#define BL_VERSION         0x0100u

/* Define to accept broadcast frames (//KEYWORD…) addressed to no specific
 * node. Comment out to require host frames to be addressed to BL_DCN_ADDRESS. */
/* #define BL_ACCEPT_BROADCAST */

/* ---- Clock and port hooks ---- */

/* Configure the CPU clock so it matches the F_CPU passed by the Makefile.
 * If the device boots up already at the right clock, leave this undefined. */
/* #define BL_CLOCK_INIT()  do { ... } while (0) */

/* Set up GPIO direction, pull-ups, and idle levels for any pin the
 * bootloader needs: UART line, trigger switch/jumper, optional LEDs. */
/* #define BL_PORT_INIT()   do { ... } while (0) */

/* Expression that evaluates to non-zero when the operator has put the
 * device into update mode via the hardware trigger. Read whatever pin
 * or pin combination signals "enter bootloader". If this product has no
 * hardware trigger and relies on app-CRC-invalid only, leave it
 * undefined (the bootloader will then only run when the app is missing
 * or corrupt). */
/* #define BL_TRIGGER_ASSERTED()   (your pin test here) */

#endif /* BL_PRODUCT_CONFIG_H */
