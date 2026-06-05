#include "bl_nvm.h"
#include "bl_config.h"
#include <avr/io.h>
#include <avr/pgmspace.h>

/*
 * The AVR EA NVMCTRL self-programming flow (per datasheet §11.3.2):
 *
 *   Page erase only:
 *     1. Wait until NVMCTRL.STATUS.FLBUSY == 0
 *     2. Dummy SPM to any byte in the target page (latches address)
 *     3. CCP-unlock (SPM key) + write CMD = FLPER
 *     4. Wait for FLBUSY == 0
 *
 *   Page write (page must already be erased):
 *     1. Wait until FLBUSY == 0
 *     2. SPM each word of the page into the page buffer
 *     3. CCP-unlock (SPM key) + write CMD = FLPW
 *     4. Wait for FLBUSY == 0
 *
 * The Z-pointer (R30:R31) holds the byte address. SPM writes the word in
 * R0:R1 into the page buffer slot determined by Z. R1 must be restored to 0
 * after SPM because the compiler relies on r1==0 as an ABI invariant.
 */

static inline void nvm_nop_cmd(void)
{
    _PROTECTED_WRITE_SPM(NVMCTRL.CTRLA, NVMCTRL_CMD_NOCMD_gc);
}

static inline void spm_write_word(uint16_t addr, uint16_t word)
{
    __asm__ __volatile__ (
        "push r0       \n\t"
        "movw r0,  %A1 \n\t"   /* data → r0:r1                    */
        "movw r30, %A0 \n\t"   /* addr → Z                        */
        "spm           \n\t"   /* store word into page buffer     */
        "clr  r1       \n\t"   /* restore r1==0 invariant         */
        "pop  r0       \n\t"
        :
        : "r" (addr), "r" (word)
        : "r30", "r31"
    );
}

static bl_nvm_status_t map_status(void)
{
    uint8_t err = (uint8_t)((NVMCTRL.STATUS & NVMCTRL_ERROR_gm) >> NVMCTRL_ERROR_gp);
    NVMCTRL.STATUS = (uint8_t)NVMCTRL_ERROR_gm;  /* W1C the error field */
    switch (err) {
        case 0: return BL_NVM_OK;
        case 2: return BL_NVM_ERR_WRITEPROTECT;
        case 3: return BL_NVM_ERR_CMDCOLLISION;
        case 4: return BL_NVM_ERR_WRONGSECTION;
        default: return BL_NVM_ERR_UNKNOWN;
    }
}

static void wait_flash_ready(void)
{
    while (NVMCTRL.STATUS & NVMCTRL_FLBUSY_bm) { /* spin */ }
}

void bl_nvm_init(void)
{
    /* Default CTRLB: all write-protect bits clear, FLMAP=SECTION0,
     * FLMAPLOCK=0. Required so the bootloader can write APPCODE. The
     * register is CCP-protected with the IOREG key. */
    _PROTECTED_WRITE(NVMCTRL.CTRLB, 0x00);
    NVMCTRL.STATUS = (uint8_t)NVMCTRL_ERROR_gm;  /* clear any stale error */
}

bool bl_nvm_is_busy(void)
{
    return (NVMCTRL.STATUS & NVMCTRL_FLBUSY_bm) != 0;
}

bl_nvm_status_t bl_nvm_page_erase(uint16_t addr)
{
    wait_flash_ready();
    nvm_nop_cmd();

    /* Dummy SPM to a word in the target page sets the address for FLPER. */
    spm_write_word(addr, 0xFFFFu);

    _PROTECTED_WRITE_SPM(NVMCTRL.CTRLA, NVMCTRL_CMD_FLPER_gc);
    wait_flash_ready();
    return map_status();
}

bl_nvm_status_t bl_nvm_erase_range(uint16_t start_addr, uint16_t end_addr)
{
    if (start_addr > end_addr) return BL_NVM_ERR_UNKNOWN;
    uint16_t a = start_addr;
    for (;;) {
        bl_nvm_status_t s = bl_nvm_page_erase(a);
        if (s != BL_NVM_OK) return s;
        if (a >= end_addr) break;
        a = (uint16_t)(a + BL_PAGE_SIZE);
    }
    return BL_NVM_OK;
}

bl_nvm_status_t bl_nvm_page_write(uint16_t addr, const uint8_t *data)
{
    wait_flash_ready();
    nvm_nop_cmd();

    /* Fill page buffer one word at a time. The page buffer is cleared by
     * the previous Page Erase, so we only fill — no buffer-clear command. */
    for (uint16_t i = 0; i < (uint16_t)BL_PAGE_SIZE; i = (uint16_t)(i + 2)) {
        uint16_t w = (uint16_t)data[i] | (uint16_t)((uint16_t)data[i + 1] << 8);
        spm_write_word((uint16_t)(addr + i), w);
    }

    _PROTECTED_WRITE_SPM(NVMCTRL.CTRLA, NVMCTRL_CMD_FLPW_gc);
    wait_flash_ready();
    return map_status();
}

bl_nvm_status_t bl_nvm_write_crc_trailer(uint16_t crc)
{
    /* The trailer lives in the last page of APPCODE at BL_APP_CRC_ADDR.
     * We need to preserve the rest of that page. Read the existing page
     * into RAM, patch the trailer bytes, erase, then rewrite the page. */
    uint8_t buf[BL_PAGE_SIZE];
    uint16_t page_base = (uint16_t)(BL_APP_CRC_ADDR & ~(uint16_t)(BL_PAGE_SIZE - 1));
    uint16_t offset    = (uint16_t)(BL_APP_CRC_ADDR - page_base);

    for (uint16_t i = 0; i < (uint16_t)BL_PAGE_SIZE; i++) {
        buf[i] = pgm_read_byte((uint16_t)(page_base + i));
    }
    buf[offset]     = (uint8_t)(crc & 0xFF);
    buf[offset + 1] = (uint8_t)(crc >> 8);

    bl_nvm_status_t s = bl_nvm_page_erase(page_base);
    if (s != BL_NVM_OK) return s;
    return bl_nvm_page_write(page_base, buf);
}
