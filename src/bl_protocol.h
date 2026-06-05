#ifndef BL_PROTOCOL_H
#define BL_PROTOCOL_H

/*
 * DCN-AVR-EA-Bootloader wire protocol.
 *
 * Rides on the existing DCN framing used by the application firmware so
 * the RS-485 bus can be shared. The bootloader only ever runs when the
 * operator has explicitly put the device into update mode (hardware
 * trigger) or when the application image is invalid — there is no
 * in-band "enter" command.
 *
 * Frame format (compatible with the application's DCN protocol):
 *
 *     /FFTT:KEYWORD[ payload...]:XX\r
 *
 *   FF      = 2 hex digits, sender address (host is conventionally 00)
 *   TT      = 2 hex digits, target address (BL_DCN_ADDRESS, default 0xBB)
 *   KEYWORD = ASCII opcode
 *   payload = opcode-specific, space-separated fields. No commas appear
 *             anywhere in a bootloader frame, so the application's
 *             comma-tokenized parser sees the whole thing as one
 *             oversized keyword that fails to match any command, and
 *             silently discards it. Other devices on the bus are
 *             undisturbed beyond transient receive-buffer churn.
 *   XX      = 2-char placeholder check field (compatible with app protocol)
 *   \r      = 0x0D
 *
 * Broadcast (//KEYWORD...) is accepted only if BL_ACCEPT_BROADCAST is
 * defined by the product config.
 *
 * Opcodes
 * -------
 *
 *   BLINFO   request:   /00BB:BLINFO:XX\r
 *            response:  /BB00:BLACK BLINFO DDDDDD PPPP SSSS EEEE TTTT VVVV C:XX\r
 *                DDDDDD = SIGROW.DEVICEID[0..2] in hex
 *                PPPP   = flash page size in bytes (hex)
 *                SSSS   = BL_APP_START (hex)
 *                EEEE   = BL_FLASH_END (hex)
 *                TTTT   = BL_APP_CRC_ADDR (hex)
 *                VVVV   = bootloader version (hex)
 *                C      = '1' if current app CRC valid, '0' if invalid
 *
 *   BLERASE  request:   /00BB:BLERASE SSSS EEEE:XX\r
 *                Erases inclusive byte range [SSSS, EEEE], page-aligned.
 *            response:  /BB00:BLACK BLERASE:XX\r       on success
 *                       /BB00:BLNAK R SSSS:XX\r        on error
 *
 *   BLDATA   request:   /00BB:BLDATA AAAA SS <hex*2*PAGE> CCCC:XX\r
 *                AAAA = page byte address (page-aligned)
 *                SS   = sequence number, 8-bit hex, wraps
 *                <hex*2*PAGE> = page bytes encoded as 2-char-per-byte hex,
 *                               no separators between bytes
 *                CCCC = CRC-16/CCITT-FALSE of the raw page bytes
 *            response:  /BB00:BLACK BLDATA AAAA SS:XX\r       on success
 *                       /BB00:BLNAK R AAAA:XX\r               on error
 *
 *   BLVERIFY request:   /00BB:BLVERIFY CCCC:XX\r
 *                CCCC = CRC-16/CCITT-FALSE of flash range
 *                       [BL_APP_START, BL_APP_CRC_ADDR), computed by host.
 *                On success the bootloader writes CCCC to BL_APP_CRC_ADDR
 *                so the next reset will boot the application.
 *            response:  /BB00:BLACK BLVERIFY CCCC:XX\r        on success
 *                       /BB00:BLNAK V XXXX:XX\r               where XXXX
 *                       is the CRC the bootloader actually computed.
 *
 *   BLRESET  request:   /00BB:BLRESET:XX\r
 *            response:  /BB00:BLACK BLRESET:XX\r
 *                       Then a watchdog reset is triggered, returning the
 *                       device to its normal reset path.
 *
 * NAK reason codes (single letter in field R):
 *   A = address out of range / not in APPCODE
 *   B = address not page-aligned
 *   E = NVMCTRL hardware error during erase/write
 *   C = payload CRC mismatch
 *   L = payload length wrong for declared page
 *   S = unexpected sequence number
 *   V = whole-image verify CRC mismatch
 *   P = protocol parse error (bad hex, malformed field)
 */

#define BL_STARTCHAR       '/'
#define BL_ENDCHAR         0x0D
#define BL_SEPARATOR       ':'
#define BL_RC_PLACEHOLDER  "XX"

#define BL_KW_INFO    "BLINFO"
#define BL_KW_ERASE   "BLERASE"
#define BL_KW_DATA    "BLDATA"
#define BL_KW_VERIFY  "BLVERIFY"
#define BL_KW_RESET   "BLRESET"

#define BL_KW_ACK     "BLACK"
#define BL_KW_NAK     "BLNAK"

#endif /* BL_PROTOCOL_H */
