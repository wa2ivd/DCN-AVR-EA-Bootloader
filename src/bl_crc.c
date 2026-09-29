#include "bl_crc.h"
#include <avr/pgmspace.h>

uint16_t bl_crc16_init(void) { return 0xFFFFu; }

uint16_t bl_crc16_update(uint16_t crc, uint8_t byte)
{
    crc ^= (uint16_t)byte << 8;
    for (uint8_t i = 0; i < 8; i++) {
        if (crc & 0x8000u) crc = (uint16_t)(crc << 1) ^ 0x1021u;
        else               crc = (uint16_t)(crc << 1);
    }
    return crc;
}

uint16_t bl_crc16_block(uint16_t crc, const uint8_t *buf, size_t len)
{
    while (len--) crc = bl_crc16_update(crc, *buf++);
    return crc;
}

/*
 * bl_crc16_flash_range() — CRC-16/CCITT-FALSE over a span of program flash.
 *
 * Reads bytes directly from flash via pgm_read_byte() (flash is not in the
 * normal data address space on AVR) and folds each one into the running CRC.
 * Seeds with bl_crc16_init() internally, so it returns a complete CRC — not
 * a partial accumulator to be chained.
 *
 * The range is HALF-OPEN: [start_addr, end_addr). start_addr is included;
 * end_addr is NOT — it is the first address past the region. This makes the
 * byte count simply (end_addr - start_addr), and lets adjacent regions abut
 * without overlap (one region's end_addr equals the next region's start_addr).
 * Example: to cover the 0x2000 bytes at 0x0000..0x1FFF, pass
 * start_addr = 0x0000, end_addr = 0x2000.
 */
uint16_t bl_crc16_flash_range(uint16_t start_addr, uint16_t end_addr)
{
    uint16_t crc = bl_crc16_init();
    for (uint16_t a = start_addr; a < end_addr; a++) {
        crc = bl_crc16_update(crc, pgm_read_byte(a));
    }
    return crc;
}
