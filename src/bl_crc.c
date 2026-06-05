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

uint16_t bl_crc16_flash_range(uint16_t start_addr, uint16_t end_addr)
{
    uint16_t crc = bl_crc16_init();
    for (uint16_t a = start_addr; a < end_addr; a++) {
        crc = bl_crc16_update(crc, pgm_read_byte(a));
    }
    return crc;
}
