#ifndef BL_CRC_H
#define BL_CRC_H

#include <stdint.h>
#include <stddef.h>

/* CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflect, no final xor.
 * Used for both the per-page BLDATA check and the whole-image trailer. */

uint16_t bl_crc16_init(void);
uint16_t bl_crc16_update(uint16_t crc, uint8_t byte);
uint16_t bl_crc16_block(uint16_t crc, const uint8_t *buf, size_t len);

/* Convenience: compute CRC over a flash range via pgm_read. */
uint16_t bl_crc16_flash_range(uint16_t start_addr, uint16_t end_addr);

#endif /* BL_CRC_H */
