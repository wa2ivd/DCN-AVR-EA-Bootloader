#ifndef BL_UART_H
#define BL_UART_H

#include <stdbool.h>
#include <stdint.h>

/* Minimal polled UART driver for the bootloader.
 *
 * Configuration (instance, pin routing, half-duplex / RS-485 mode) is
 * supplied by the product config via the BL_UART_* macros. The driver
 * intentionally exposes only what the bootloader needs — no callbacks,
 * no ring buffers, no error recovery beyond clearing the status flags.
 */

void bl_uart_init(void);
void bl_uart_deinit(void);     /* return USART registers to reset state */
bool bl_uart_rx_ready(void);
uint8_t bl_uart_read(void);
void bl_uart_write(uint8_t b);
void bl_uart_write_str(const char *s);
void bl_uart_write_hex8(uint8_t v);
void bl_uart_write_hex16(uint16_t v);
void bl_uart_flush_tx(void);   /* block until TX shift register is empty */

#endif /* BL_UART_H */
