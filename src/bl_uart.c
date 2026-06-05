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

/*
 * Baud register value. The AVR EA USART runs at F_CPU and the equation is
 *   BAUD = (64 * F_CPU) / (S * f_baud), S = 16 for asynchronous normal mode.
 * Using integer math with rounding to match what MCC generates.
 */
#define BL_BAUD_REG  ((uint16_t)((((uint32_t)F_CPU * 64UL) + (8UL * (uint32_t)BL_BAUD)) / (16UL * (uint32_t)BL_BAUD)))

void bl_uart_init(void)
{
    BL_USART.BAUD = BL_BAUD_REG;

    /* CTRLA: optional half-duplex/loopback mode chosen by product config.
     * Bit 3 (LBME) and the RS485 mode field live here. */
#ifdef BL_USART_CTRLA
    BL_USART.CTRLA = BL_USART_CTRLA;
#else
    BL_USART.CTRLA = 0;
#endif

    /* CTRLB: RXEN | TXEN, plus optional ODME (open-drain mode) for single-wire.
     * RXMODE = NORMAL (0). */
#ifdef BL_USART_CTRLB
    BL_USART.CTRLB = BL_USART_CTRLB;
#else
    BL_USART.CTRLB = USART_RXEN_bm | USART_TXEN_bm;
#endif

    /* CTRLC: 8N1 asynchronous (CHSIZE_8BIT_gc | PMODE_DISABLED | 1 stop) */
    BL_USART.CTRLC = USART_CHSIZE_8BIT_gc;
}

void bl_uart_deinit(void)
{
    BL_USART.CTRLB = 0;
    BL_USART.CTRLA = 0;
    BL_USART.CTRLC = 0;
    BL_USART.BAUD  = 0;
}

bool bl_uart_rx_ready(void)
{
    return (BL_USART.STATUS & USART_RXCIF_bm) != 0;
}

uint8_t bl_uart_read(void)
{
    /* Reading RXDATAH first would latch error bits; the bootloader does not
     * recover from frame errors — a corrupted frame just fails to parse and
     * the host retries. Read the data byte and move on. */
    return BL_USART.RXDATAL;
}

static void uart_tx_wait_ready(void)
{
    while (!(BL_USART.STATUS & USART_DREIF_bm)) { /* spin */ }
}

void bl_uart_write(uint8_t b)
{
    uart_tx_wait_ready();
    BL_USART.TXDATAL = b;
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
    while (!(BL_USART.STATUS & USART_TXCIF_bm)) { /* spin */ }
    BL_USART.STATUS = USART_TXCIF_bm;  /* W1C the flag for next round */
}
