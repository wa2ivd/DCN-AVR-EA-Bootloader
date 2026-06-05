#ifndef BL_NVM_H
#define BL_NVM_H

#include <stdint.h>
#include <stdbool.h>

/* Lean NVM driver for self-programming APPCODE from the BOOT section.
 *
 * Per the AVR EA NVMCTRL chapter: code executing in the BOOT section is
 * allowed to write APPCODE, APPDATA, EEPROM, and USERROW. We only need
 * APPCODE writes for the bootloader's purpose, so the API is narrow.
 *
 * All addresses are byte addresses in the code-space map (0x0000–0xFFFF
 * on the 64K parts). The SPM-based writes use the Z-pointer directly
 * so no FLMAP juggling is needed.
 */

typedef enum {
    BL_NVM_OK = 0,
    BL_NVM_ERR_WRITEPROTECT,
    BL_NVM_ERR_CMDCOLLISION,
    BL_NVM_ERR_WRONGSECTION,
    BL_NVM_ERR_UNKNOWN,
} bl_nvm_status_t;

void bl_nvm_init(void);

/* Erase a single page (BL_PAGE_SIZE bytes) whose first byte lives at addr.
 * addr must be page-aligned and inside APPCODE. */
bl_nvm_status_t bl_nvm_page_erase(uint16_t addr);

/* Erase a contiguous page range [start_addr, end_addr] inclusive.
 * Both endpoints must be page-aligned. */
bl_nvm_status_t bl_nvm_erase_range(uint16_t start_addr, uint16_t end_addr);

/* Fill the page buffer with `data` (must be exactly BL_PAGE_SIZE bytes)
 * and commit it to flash at addr. addr must be page-aligned. The caller
 * is responsible for having erased the page first (bl_nvm_page_erase). */
bl_nvm_status_t bl_nvm_page_write(uint16_t addr, const uint8_t *data);

/* Write the two-byte CRC trailer at BL_APP_CRC_ADDR. Reads the existing
 * page, replaces the trailer bytes, erases, and rewrites. The caller has
 * already verified the body bytes match `crc`. */
bl_nvm_status_t bl_nvm_write_crc_trailer(uint16_t crc);

bool bl_nvm_is_busy(void);

#endif /* BL_NVM_H */
