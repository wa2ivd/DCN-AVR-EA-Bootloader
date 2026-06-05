#include "bl_config.h"
#include "bl_uart.h"
#include "bl_nvm.h"
#include "bl_dcn.h"
#include "bl_crc.h"
#include <avr/io.h>
#include <avr/pgmspace.h>
#include <avr/interrupt.h>
#include <util/delay.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * DCN-AVR-EA-Bootloader entry point.
 *
 * Lives at FLASHSTART (0x0000) in the BOOT section. On reset the CPU always
 * starts here regardless of CPUINT.CTRLA.IVSEL. We never enable interrupts;
 * we leave IVSEL at its default (0) so that when control transfers to the
 * application at BL_APP_START, the application's own vector table — placed
 * at the start of APPCODE by its linker — is what the CPU uses for IRQs.
 * That removes any need for vector-trampoline tricks.
 */

/* Whole-image CRC stored as a 2-byte little-endian word at BL_APP_CRC_ADDR.
 * Computed over the inclusive range [BL_APP_START, BL_APP_CRC_ADDR). */
bool bl_app_image_valid(void)
{
    uint8_t lo = pgm_read_byte((uint16_t)BL_APP_CRC_ADDR);
    uint8_t hi = pgm_read_byte((uint16_t)(BL_APP_CRC_ADDR + 1));
    uint16_t stored = (uint16_t)((uint16_t)hi << 8 | lo);

    /* An erased trailer reads 0xFFFF — treat that as definitively invalid
     * even if a real CRC happens to compute to 0xFFFF, to keep "freshly
     * erased flash" and "valid image" unambiguous. */
    if (stored == 0xFFFFu) return false;

    uint16_t computed = bl_crc16_flash_range((uint16_t)BL_APP_START,
                                             (uint16_t)BL_APP_CRC_ADDR);
    return computed == stored;
}

static void disable_watchdog(void)
{
    /* WDT.CTRLA is CCP-protected with the IOREG key. Default fuse value
     * leaves WDT disabled, but if a previous app set the WDT lock fuse
     * (WDTCFG locks the WDT after the first runtime write), this is a
     * no-op and the bootloader's NVM ops must complete inside the window.
     * BL_PAGE_SIZE writes finish in single-digit ms, well within any
     * reasonable WDT period. */
    _PROTECTED_WRITE(WDT.CTRLA, 0);
}

static void clock_init(void)
{
    /* Product hook — defaults to "leave the post-reset configuration alone"
     * which is OSCHF / DIV6 ≈ 3.3 MHz on a 20 MHz-fuse part. If the product
     * baud requires a specific clock, define BL_CLOCK_INIT() in the
     * product config to set it up. */
#ifdef BL_CLOCK_INIT
    BL_CLOCK_INIT();
#endif
}

static void port_init(void)
{
    /* Product hook for all GPIO pin setup the bootloader needs: UART pin
     * direction and idle state, trigger-input pull-ups, anything else
     * required before reading the entry trigger or talking on the bus. */
#ifdef BL_PORT_INIT
    BL_PORT_INIT();
#endif
}

static bool entry_trigger_asserted(void)
{
#ifdef BL_TRIGGER_ASSERTED
    return (BL_TRIGGER_ASSERTED()) ? true : false;
#else
    return false;   /* product config with no trigger == only-bad-image entry */
#endif
}

static __attribute__((noreturn)) void jump_to_app(void)
{
    cli();

    /* Restore peripherals to a quiet state so the application's own init
     * code sees something close to a fresh reset. */
    bl_uart_deinit();
    _PROTECTED_WRITE(NVMCTRL.CTRLB, 0x00);

    /* JMP takes an immediate code-space byte address; the assembler emits
     * the word-address encoding. Verified by inspecting the .lss output. */
    asm volatile ("jmp %0" :: "i"(BL_APP_START));
    __builtin_unreachable();
}

static __attribute__((noreturn)) void wait_for_wdt_reset(void)
{
    /* Triggered after acknowledging BLRESET. Shortest available period so
     * the reset happens promptly. The ACK has already been TX-flushed by
     * the DCN responder. */
    _PROTECTED_WRITE(WDT.CTRLA, WDT_PERIOD_8CLK_gc);
    for (;;) { /* loop until the watchdog fires */ }
}

int main(void)
{
    cli();
    disable_watchdog();
    clock_init();
    bl_nvm_init();
    port_init();

    /* Brief settle so any pull-ups enabled in trigger_pins_init() have time
     * to charge before we sample. Pull-ups on AVR are ~20–50 kΩ — a few
     * tens of microseconds is plenty for typical board capacitance. */
    _delay_us(50);

    bool stay = entry_trigger_asserted() || !bl_app_image_valid();

    if (!stay) {
        jump_to_app();
    }

    bl_uart_init();

    for (;;) {
        (void)bl_dcn_service();
        if (bl_dcn_reset_requested()) {
            wait_for_wdt_reset();
        }
    }
}
