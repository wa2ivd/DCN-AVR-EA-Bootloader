#include "bl_uart.h"
#include "bl_config.h"
#include <avr/io.h>

/* The product config picks the USART instance via BL_USART. We default to
 * USART0 if it didn't specify one. Pin routing (PORTMUX) is the product's
 * responsibility — we don't touch it here so the same MCU pinout used by the
 * application keeps working. */
#ifndef BL_USART
#  define BL_USART  USART0
#endif

/* Dual-port option: when the product defines BL_USART_USB (a second USART
 * wired to a USB-to-serial bridge) plus the BL_USB_PRESENT() pin test,
 * bl_uart_init() samples the bridge's "configured by a host" line once and
 * latches the whole session onto that port: USB present → BL_USART_USB,
 * otherwise → BL_USART. The unselected USART is never enabled, so it cannot
 * receive or transmit anything. Products without a second port compile the
 * original direct-register code at zero cost. */
#ifdef BL_USART_USB
#  ifndef BL_USB_PRESENT
#    error "BL_USART_USB requires BL_USB_PRESENT() in the product config"
#  endif
#  ifndef BL_USART_CTRLA
#    define BL_USART_CTRLA      0
#  endif
#  ifndef BL_USART_CTRLB
#    define BL_USART_CTRLB      (USART_RXEN_bm | USART_TXEN_bm)
#  endif
#  ifndef BL_USART_USB_CTRLA
#    define BL_USART_USB_CTRLA  0
#  endif
#  ifndef BL_USART_USB_CTRLB
#    define BL_USART_USB_CTRLB  (USART_RXEN_bm | USART_TXEN_bm)
#  endif

/* Points at the primary port before init: jump_to_app() calls
 * bl_uart_deinit() even on the no-stay path where bl_uart_init() never ran,
 * and writing reset values to the untouched primary USART is harmless. */
static USART_t *bl_usart = &BL_USART;
#  define UARTREG  (*bl_usart)
#else
#  define UARTREG  BL_USART
#endif

/*
 * Baud register value. The AVR EA USART runs at F_CPU and the equation is
 *   BAUD = (64 * F_CPU) / (S * f_baud), S = 16 for asynchronous normal mode.
 * Using integer math with rounding to match what MCC generates.
 */
#define BL_BAUD_REG  ((uint16_t)((((uint32_t)F_CPU * 64UL) + (8UL * (uint32_t)BL_BAUD)) / (16UL * (uint32_t)BL_BAUD)))

void bl_uart_init(void)
{
#ifdef BL_USART_USB
    /* Latch the port once per session. USBCFG needs settle time after
     * power-up (BL_USB_SETTLE_MS, handled by main before calling us). A
     * cable change mid-session takes effect on the next reset. */
    bool usb = BL_USB_PRESENT() ? true : false;
    bl_usart = usb ? &BL_USART_USB : &BL_USART;

    UARTREG.BAUD  = BL_BAUD_REG;
    /* CTRLA/CTRLB per port: the primary port may want LBME / RS-485 / ODME
     * for a half-duplex bus; the USB bridge is plain full duplex. */
    UARTREG.CTRLA = usb ? (BL_USART_USB_CTRLA) : (BL_USART_CTRLA);
    UARTREG.CTRLB = usb ? (BL_USART_USB_CTRLB) : (BL_USART_CTRLB);
#else
    UARTREG.BAUD = BL_BAUD_REG;

    /* CTRLA: optional half-duplex/loopback mode chosen by product config.
     * Bit 3 (LBME) and the RS485 mode field live here. */
#ifdef BL_USART_CTRLA
    UARTREG.CTRLA = BL_USART_CTRLA;
#else
    UARTREG.CTRLA = 0;
#endif

    /* CTRLB: RXEN | TXEN, plus optional ODME (open-drain mode) for single-wire.
     * RXMODE = NORMAL (0). */
#ifdef BL_USART_CTRLB
    UARTREG.CTRLB = BL_USART_CTRLB;
#else
    UARTREG.CTRLB = USART_RXEN_bm | USART_TXEN_bm;
#endif
#endif /* BL_USART_USB */

    /* CTRLC: 8N1 asynchronous (CHSIZE_8BIT_gc | PMODE_DISABLED | 1 stop) */
    UARTREG.CTRLC = USART_CHSIZE_8BIT_gc;
}

void bl_uart_deinit(void)
{
    UARTREG.CTRLB = 0;
    UARTREG.CTRLA = 0;
    UARTREG.CTRLC = 0;
    UARTREG.BAUD  = 0;
}

bool bl_uart_rx_ready(void)
{
    return (UARTREG.STATUS & USART_RXCIF_bm) != 0;
}

uint8_t bl_uart_read(void)
{
    /* Reading RXDATAH first would latch error bits; the bootloader does not
     * recover from frame errors — a corrupted frame just fails to parse and
     * the host retries. Read the data byte and move on. */
    return UARTREG.RXDATAL;
}

static void uart_tx_wait_ready(void)
{
    while (!(UARTREG.STATUS & USART_DREIF_bm)) { /* spin */ }
}

void bl_uart_write(uint8_t b)
{
    uart_tx_wait_ready();
    UARTREG.TXDATAL = b;
}

void bl_uart_write_str(const char *s)
{
    while (*s) bl_uart_write((uint8_t)*s++);
}

static const char hexchars[] = "0123456789ABCDEF";

void bl_uart_write_hex8(uint8_t v)
{
    bl_uart_write((uint8_t)hexchars[(v >> 4) & 0xF]);
    bl_uart_write((uint8_t)hexchars[v & 0xF]);
}

void bl_uart_write_hex16(uint16_t v)
{
    bl_uart_write_hex8((uint8_t)(v >> 8));
    bl_uart_write_hex8((uint8_t)(v & 0xFF));
}

void bl_uart_flush_tx(void)
{
    /* Wait for the last byte to clear the shift register. Important for
     * half-duplex bus topologies where we hand the line back to the host
     * immediately after this returns. */
    while (!(UARTREG.STATUS & USART_TXCIF_bm)) { /* spin */ }
    UARTREG.STATUS = USART_TXCIF_bm;  /* W1C the flag for next round */
}
