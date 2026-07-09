#ifndef BL_PRODUCT_CONFIG_H
#define BL_PRODUCT_CONFIG_H

/*
 * Product config: SPS-1 (Smart Power Switch, AVR64EA28)
 *
 * Bootloader entry trigger: 4-bit BCD address switch on PORTC bits 0–3.
 * Switch is active-low (pull-ups internal), so the position-15 selection
 * reads back as 0x0 on the pins and inverts to 0xF in dcn_addr terms —
 * which is what the application's OPERATION_Initialize.c calls "address
 * 15". That position is reserved for the bootloader.
 *
 * UART: USART0, 9600 baud, 8N1, single-wire half-duplex on PA0
 * (open-drain TX with internal loopback to RX). Matches the application's
 * RS-485 / DCN bus setup.
 *
 * USB: a USB-to-serial bridge on USART2 (PF0/PF1) is the alternate host
 * port. The bridge's USBCFG output (PA1) is high once a USB host has
 * configured it; bl_uart_init() samples it once — after BL_USB_SETTLE_MS —
 * and runs the whole session on USART2 (USB) or USART0 (RS-485 bus). The
 * unselected USART is never enabled.
 */

#include <avr/io.h>

#define BL_PRODUCT_NAME    "SPS-1"

/* Address the bootloader responds to. Host targets the device with /00BB:…
 * Application traffic to its own (BCD-set) address is on a different
 * address byte and is silently ignored by the bootloader. */
#define BL_DCN_ADDRESS     0xBBu

/* UART setup */
#define BL_USART           USART0
#define BL_BAUD            9600UL
/* CTRLA: LBME=1, RS485 field = 0x1 (external driver), other bits 0.
 * Same value the application's USART0_Initialize() programs. */
#define BL_USART_CTRLA     0x09u
/* CTRLB: RXEN | TXEN | ODME (open-drain) for single-wire half-duplex. */
#define BL_USART_CTRLB     (USART_RXEN_bm | USART_TXEN_bm | USART_ODME_bm)

/* ---- USB serial port (USART2) ---- */

/* The bridge is wired to USART2's default pin route (PORTMUX.USARTROUTEB
 * reset value — no PORTMUX write needed):
 *   PF0 = TXD2 → bridge RX (data toward the USB host)
 *   PF1 = RXD2 ← bridge TX (data from the USB host)
 * Dedicated TX/RX lines, full duplex: no LBME, no RS-485 mode, no ODME.
 * Baud and 8N1 framing are shared with USART0 (BL_BAUD / CTRLC in
 * bl_uart.c), so both ports speak the identical protocol setup.
 *
 * USBCFG (PA1) is also USART0's default RXD pin, but USART0 runs in
 * loopback mode (LBME — RX is tied internally to PA0) and never samples
 * PA1, so the two uses don't conflict. */
#define BL_USART_USB        USART2
#define BL_USART_USB_CTRLA  0x00u
#define BL_USART_USB_CTRLB  (USART_RXEN_bm | USART_TXEN_bm)
#define BL_USB_PRESENT()    ((VPORTA.IN & PIN1_bm) != 0)

/* The bridge needs time after power-up to enumerate with a host before
 * USBCFG is meaningful. bl_main waits this long before sampling it. */
#define BL_USB_SETTLE_MS    500u

/* Memory layout — must agree with bl_fuses.c BL_FUSE_BOOTSIZE.
 *   BOOTSIZE = 0x10 → BOOT region is 0x0000–0x0FFF (4 KB).
 *   APPCODE starts immediately after, at BL_APP_START. */
#define BL_APP_START       0x1000u
#define BL_FLASH_END       0xFFFFu
#define BL_PAGE_SIZE       128u
#define BL_APP_CRC_ADDR    0xFFFEu          /* last 2 bytes hold the CRC trailer */

#define BL_VERSION         0x0100u           /* bootloader v1.00 */

/* Allow broadcast frames (//KEYWORD…) so a host that doesn't know the
 * bootloader's fixed address can still drive the update. Comment this
 * out for stricter address discipline on a multi-drop bus. */
#define BL_ACCEPT_BROADCAST

/* ---- Clock / port hooks ---- */

/* Match the application's clock: OSCHF (20 MHz from OSCCFG fuse) divided
 * by 2 → 10 MHz CPU. F_CPU is set by the Makefile via -DF_CPU=10000000UL
 * so avr-libc <util/delay.h> and the baud calculation agree. */
#define BL_CLOCK_INIT()  do {                                              \
    _PROTECTED_WRITE(CLKCTRL.MCLKCTRLA, CLKCTRL_CLKSEL_OSCHF_gc);          \
    _PROTECTED_WRITE(CLKCTRL.MCLKCTRLB, CLKCTRL_PDIV_DIV2_gc |             \
                                        CLKCTRL_PEN_bm);                   \
    while (!(CLKCTRL.MCLKSTATUS & CLKCTRL_OSCHFS_bm)) { /* settle */ }     \
} while (0)

/* Port setup:
 *   PORTA.PIN0 — USART0 TXD/RXD (open-drain single-wire). Idle high,
 *                direction = output.
 *   PORTA.PIN1 — USBCFG status from the USB bridge. Input, no pull-up
 *                (the bridge actively drives it; a pull-up could fake
 *                "USB present" if the bridge is unpowered).
 *   PORTA.PIN3 — USART0 XDIR0 (RS-485 EXT driver-enable). Must be
 *                DIR=output for the USART's RS485-mode alternate-function
 *                override to take control of the pin; otherwise the
 *                external transceiver is stuck in RX and bootloader
 *                responses never reach the bus.
 *   PORTF.PIN0 — USART2 TXD. Idle high, direction = output.
 *   PORTF.PIN1 — USART2 RXD. Input (reset default, nothing to write).
 *   PORTC.PIN[0..3] — DCN address BCD switch, input with pull-up.
 */
#define BL_PORT_INIT()  do {                                               \
    PORTA.OUT       = (uint8_t)(PORTA.OUT       |  0x01);                  \
    PORTA.DIR       = (uint8_t)(PORTA.DIR       |  0x09);                  \
    PORTA.PIN1CTRL  = 0;                                                   \
    PORTF.OUT       = (uint8_t)(PORTF.OUT       |  0x01);                  \
    PORTF.DIR       = (uint8_t)(PORTF.DIR       |  0x01);                  \
    PORTC.DIR       = (uint8_t)(PORTC.DIR       & ~0x0F);                  \
    PORTC.PIN0CTRL  = PORT_PULLUPEN_bm;                                    \
    PORTC.PIN1CTRL  = PORT_PULLUPEN_bm;                                    \
    PORTC.PIN2CTRL  = PORT_PULLUPEN_bm;                                    \
    PORTC.PIN3CTRL  = PORT_PULLUPEN_bm;                                    \
} while (0)

/* Trigger position: a logical DCN address of 0xF (all four switch
 * positions on). PORTC pins read inverted, so the switch wired for
 * address 0xF presents 0x0 on the pins; we invert and mask before
 * comparing. */
#define BL_TRIGGER_MAGIC      0x0Fu
#define BL_TRIGGER_ASSERTED() (((uint8_t)(~VPORTC.IN) & 0x0Fu) == BL_TRIGGER_MAGIC)

#endif /* BL_PRODUCT_CONFIG_H */
