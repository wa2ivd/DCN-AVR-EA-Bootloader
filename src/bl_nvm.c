#include "bl_nvm.h"
#include "bl_config.h"
#include <avr/io.h>
#include <avr/pgmspace.h>

/*
 * The AVR EA NVMCTRL self-programming flow.
 *
 * Datasheet §11.3.2 offers two equivalent paths for accessing flash:
 *
 *   (a) Code-space via LPM/SPM with the Z-pointer.
 *   (b) Data-space via LD/ST through the 32 KB window at MAPPED_PROGMEM_START
 *       (0x8000). NVMCTRL.CTRLB.FLMAP selects which 32 KB flash section is
 *       visible in the window.
 *
 * The bootloader uses path (b). On the AVR64EA28 we saw on the bench, the
 * SPM page-buffer load was silently dropped — FLPW/FLPERW returned ERROR=0
 * but the flash page never changed, so ACKs came back without any commit.
 * Switching to plain ST writes through the mapped window made writes stick.
 *
 * Per §11.3.2.3 Option 1, the page-programming flow we use is:
 *
 *   1. wait FLBUSY=0
 *   2. CTRLA = NOOP            (clear any prior command)
 *   3. point FLMAP at the section containing the target page
 *   4. fill the page buffer by writing each byte through the mapped window
 *   5. CTRLA = FLPERW          (erase + write in one combined commit)
 *   6. wait FLBUSY=0, check STATUS.ERROR
 *
 * Page erase alone uses the same flow but with a single dummy byte write
 * at step 4 and FLPER at step 5.
 *
 * Other porting gotchas worth keeping in mind:
 *   - CTRLA needs CCP unlock with the SPM key (_PROTECTED_WRITE_SPM).
 *     CTRLB needs CCP unlock with the IOREG key (_PROTECTED_WRITE).
 *   - Use NOOP (0x01) rather than NOCMD (0x00) as the inter-command reset.
 *     On EA, NOCMD can leave the CMD state machine in a state where the
 *     next FLPER/FLPW raises CMDCOLLISION.
 *   - Do not write NVMCTRL.STATUS to "clear" ERROR. ERROR clears
 *     automatically when the next command issues; writing STATUS can
 *     itself trip CMDCOLLISION.
 */

/* The mapped flash window starts at MAPPED_PROGMEM_START (0x8000 on EA) and
 * is 32 KB wide. The low 15 bits of any byte address inside the currently-
 * mapped section index into the window. */
#ifndef MAPPED_PROGMEM_START
#  define MAPPED_PROGMEM_START 0x8000u
#endif
#define BL_MAPPED_WINDOW_MASK  0x7FFFu

static inline void nvm_nop_cmd(void)
{
    /* NOOP, not NOCMD — see the file-level comment block. */
    _PROTECTED_WRITE_SPM(NVMCTRL.CTRLA, NVMCTRL_CMD_NOOP_gc);
}

/* Aim the 0x8000+ data-space window at the 32 KB flash section containing
 * `flash_addr`. On 64 KB EA parts there are exactly two sections (0 and 1).
 * Skips the CCP-protected write if FLMAP already matches. */
static inline void nvm_point_flmap_at(uint16_t flash_addr)
{
    uint8_t want = (uint8_t)((flash_addr >= 0x8000u)
                             ? NVMCTRL_FLMAP_SECTION1_gc
                             : NVMCTRL_FLMAP_SECTION0_gc);
    uint8_t cur  = NVMCTRL.CTRLB;
    if ((cur & NVMCTRL_FLMAP_gm) == want) return;
    _PROTECTED_WRITE(NVMCTRL.CTRLB,
                     (uint8_t)((cur & (uint8_t)~NVMCTRL_FLMAP_gm) | want));
}

/* Returns a data-space pointer that aliases `flash_addr` in the mapped
 * window. The caller must have called nvm_point_flmap_at(flash_addr) first;
 * otherwise the pointer aliases a different flash byte in the wrong section. */
static inline volatile uint8_t *nvm_mapped_ptr(uint16_t flash_addr)
{
    return (volatile uint8_t *)(uint16_t)
           (MAPPED_PROGMEM_START | (flash_addr & BL_MAPPED_WINDOW_MASK));
}

static bl_nvm_status_t map_status(void)
{
    /* Don't write STATUS. ERROR is cleared automatically when the next
     * command issues; on EA, a manual W1C attempt can trip CMDCOLLISION. */
    uint8_t err = (uint8_t)((NVMCTRL.STATUS & NVMCTRL_ERROR_gm) >> NVMCTRL_ERROR_gp);
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
    /* Do not poke NVMCTRL.STATUS here. See map_status() and the file-level
     * comment for why. */
}

bool bl_nvm_is_busy(void)
{
    return (NVMCTRL.STATUS & NVMCTRL_FLBUSY_bm) != 0;
}

bl_nvm_status_t bl_nvm_page_erase(uint16_t addr)
{
    wait_flash_ready();
    nvm_nop_cmd();

    /* Per datasheet §11.3.2.4: arm FLPER first, then a single dummy ST to
     * the target page through the mapped window triggers the actual erase.
     * CMD must be set BEFORE the ST or the ST is ignored. */
    nvm_point_flmap_at(addr);
    _PROTECTED_WRITE_SPM(NVMCTRL.CTRLA, NVMCTRL_CMD_FLPER_gc);
    *nvm_mapped_ptr(addr) = 0xFFu;

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

    /* Per datasheet §11.3.2.3 Option 1: fill page buffer FIRST, then issue
     * FLPERW which commits the buffer. (Section 11.3.2.4's "CMD enables /
     * ST/SPM applies" wording reads the other way around but the explicit
     * step list in 11.3.2.3 and Microchip's own MCC reference both fill
     * the buffer before the command write.) */
    nvm_point_flmap_at(addr);
    volatile uint8_t *p = nvm_mapped_ptr(addr);
    for (uint16_t i = 0; i < (uint16_t)BL_PAGE_SIZE; i++) {
        p[i] = data[i];
    }

    _PROTECTED_WRITE_SPM(NVMCTRL.CTRLA, NVMCTRL_CMD_FLPERW_gc);
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
