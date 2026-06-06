#include "bl_dcn.h"
#include "bl_protocol.h"
#include "bl_config.h"
#include "bl_uart.h"
#include "bl_nvm.h"
#include "bl_crc.h"
#include <avr/io.h>
#include <stdbool.h>
#include <stdint.h>

/*
 * Frame buffer must hold the largest expected frame.
 *
 *   /FFTT:BLDATA AAAA SS <2*PAGE hex> CCCC:XX\r
 *      1 + 4 + 1 + 6  + 1 + 4 + 1 + 2 + 1 + (2*PAGE) + 1 + 4 + 1 + 2 + 1
 *      = 30 + 2*PAGE
 *
 * For BL_PAGE_SIZE=128 that's 286 bytes. Round up to a multiple of 32. */
#define BL_RX_BUF_SIZE ((uint16_t)(2u * (uint16_t)BL_PAGE_SIZE + 64u))

static char rx_buf[BL_RX_BUF_SIZE];
static uint16_t rx_len = 0;
static enum { RX_IDLE, RX_COLLECTING } rx_state = RX_IDLE;

static uint8_t host_addr  = 0;     /* sender of the frame currently being processed */
static uint8_t last_seq   = 0;     /* sequence of the last successfully written page */
static bool    have_seq   = false; /* false until the first BLDATA arrives */
static bool    reset_req  = false;

/* App-validity probe — declared in bl_main.c to avoid a circular include. */
extern bool bl_app_image_valid(void);

/* -------------------- hex / parse helpers -------------------- */

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static bool parse_hex_u8(const char **pp, uint8_t *out)
{
    int hi = hex_nibble((*pp)[0]);
    int lo = hex_nibble((*pp)[1]);
    if (hi < 0 || lo < 0) return false;
    *out = (uint8_t)((hi << 4) | lo);
    *pp += 2;
    return true;
}

static bool parse_hex_u16(const char **pp, uint16_t *out)
{
    uint8_t hi, lo;
    if (!parse_hex_u8(pp, &hi)) return false;
    if (!parse_hex_u8(pp, &lo)) return false;
    *out = (uint16_t)(((uint16_t)hi << 8) | lo);
    return true;
}

static bool expect_space(const char **pp, const char *end)
{
    if (*pp >= end || **pp != ' ') return false;
    (*pp)++;
    return true;
}

static bool keyword_equals(const char *p, uint16_t len, const char *expected)
{
    for (uint16_t i = 0; i < len; i++) {
        if (expected[i] == '\0' || p[i] != expected[i]) return false;
    }
    return expected[len] == '\0';
}

/* -------------------- response emitters -------------------- */

static void emit_header(void)
{
    bl_uart_write(BL_STARTCHAR);
    bl_uart_write_hex8((uint8_t)BL_DCN_ADDRESS);
    bl_uart_write_hex8(host_addr);
    bl_uart_write(BL_SEPARATOR);
}

static void emit_trailer(void)
{
    bl_uart_write(BL_SEPARATOR);
    bl_uart_write_str(BL_RC_PLACEHOLDER);
    bl_uart_write(BL_ENDCHAR);
    bl_uart_flush_tx();
}

static void emit_nak(char reason, bool has_addr, uint16_t addr)
{
    emit_header();
    bl_uart_write_str(BL_KW_NAK);
    bl_uart_write(' ');
    bl_uart_write((uint8_t)reason);
    if (has_addr) {
        bl_uart_write(' ');
        bl_uart_write_hex16(addr);
    }
    emit_trailer();
}

static void emit_ack_simple(const char *cmd_kw)
{
    emit_header();
    bl_uart_write_str(BL_KW_ACK);
    bl_uart_write(' ');
    bl_uart_write_str(cmd_kw);
    emit_trailer();
}

/* -------------------- handlers -------------------- */

static void handle_blinfo(void)
{
    emit_header();
    bl_uart_write_str(BL_KW_ACK);
    bl_uart_write(' ');
    bl_uart_write_str(BL_KW_INFO);
    bl_uart_write(' ');

    bl_uart_write_hex8(SIGROW.DEVICEID0);
    bl_uart_write_hex8(SIGROW.DEVICEID1);
    bl_uart_write_hex8(SIGROW.DEVICEID2);
    bl_uart_write(' ');
    bl_uart_write_hex16((uint16_t)BL_PAGE_SIZE);
    bl_uart_write(' ');
    bl_uart_write_hex16((uint16_t)BL_APP_START);
    bl_uart_write(' ');
    bl_uart_write_hex16((uint16_t)BL_FLASH_END);
    bl_uart_write(' ');
    bl_uart_write_hex16((uint16_t)BL_APP_CRC_ADDR);
    bl_uart_write(' ');
    bl_uart_write_hex16((uint16_t)BL_VERSION);
    bl_uart_write(' ');
    bl_uart_write(bl_app_image_valid() ? '1' : '0');

    emit_trailer();
}

static void handle_blerase(const char *p, const char *end)
{
    uint16_t s, e;
    if (!expect_space(&p, end) || !parse_hex_u16(&p, &s)) { emit_nak('P', false, 0); return; }
    if (!expect_space(&p, end) || !parse_hex_u16(&p, &e)) { emit_nak('P', false, 0); return; }

    if (s < (uint16_t)BL_APP_START || e > (uint16_t)BL_FLASH_END || e < s) {
        emit_nak('A', true, s); return;
    }
    if ((s & ((uint16_t)BL_PAGE_SIZE - 1u)) != 0 ||
        ((uint16_t)(e + 1u) & ((uint16_t)BL_PAGE_SIZE - 1u)) != 0) {
        emit_nak('B', true, s); return;
    }

    uint16_t last_page = (uint16_t)(e & (uint16_t)~(uint16_t)((uint16_t)BL_PAGE_SIZE - 1u));
    bl_nvm_status_t st = bl_nvm_erase_range(s, last_page);
    if (st != BL_NVM_OK) {
        /* For 'E' the addr field is not meaningful (the controller failed
         * on some page in the range), so we repurpose it to report the
         * bl_nvm_status_t code: 1=WRITEPROTECT, 2=CMDCOLLISION,
         * 3=WRONGSECTION, 4=UNKNOWN. Saves a firmware rebuild to find out
         * which class of failure the NVMCTRL is reporting. */
        emit_nak('E', true, (uint16_t)st);
        return;
    }

    /* New erase resets the sequence-number tracking. */
    have_seq = false;

    emit_ack_simple(BL_KW_ERASE);
}

static void handle_bldata(const char *p, const char *end)
{
    uint16_t addr;
    uint8_t  seq;
    uint16_t declared_crc;

    if (!expect_space(&p, end) || !parse_hex_u16(&p, &addr)) { emit_nak('P', false, 0); return; }
    if (!expect_space(&p, end) || !parse_hex_u8(&p, &seq))   { emit_nak('P', false, 0); return; }
    if (!expect_space(&p, end))                              { emit_nak('P', false, 0); return; }

    /* Decode 2*PAGE hex characters into a local page buffer. */
    uint8_t page_buf[BL_PAGE_SIZE];
    const char *data_start = p;
    const char *data_end   = data_start + (2u * (uint16_t)BL_PAGE_SIZE);
    if (data_end > end) { emit_nak('L', true, addr); return; }

    for (uint16_t i = 0; i < (uint16_t)BL_PAGE_SIZE; i++) {
        int hi = hex_nibble(data_start[2 * i]);
        int lo = hex_nibble(data_start[2 * i + 1]);
        if (hi < 0 || lo < 0) { emit_nak('P', true, addr); return; }
        page_buf[i] = (uint8_t)((hi << 4) | lo);
    }
    p = data_end;

    if (!expect_space(&p, end) || !parse_hex_u16(&p, &declared_crc)) {
        emit_nak('P', true, addr); return;
    }

    /* Address checks. */
    if (addr < (uint16_t)BL_APP_START || addr > (uint16_t)BL_FLASH_END) {
        emit_nak('A', true, addr); return;
    }
    if ((addr & ((uint16_t)BL_PAGE_SIZE - 1u)) != 0) {
        emit_nak('B', true, addr); return;
    }

    /* Payload CRC check (against the raw bytes, not the hex string). */
    uint16_t crc = bl_crc16_block(bl_crc16_init(), page_buf, (uint16_t)BL_PAGE_SIZE);
    if (crc != declared_crc) { emit_nak('C', true, addr); return; }

    /* Sequence number sanity. The first BLDATA after BLERASE may have any
     * value; subsequent ones must be the previous + 1 (mod 256). */
    if (have_seq && seq != (uint8_t)(last_seq + 1u)) {
        emit_nak('S', true, addr); return;
    }

    bl_nvm_status_t st = bl_nvm_page_write(addr, page_buf);
    if (st != BL_NVM_OK) { emit_nak('E', true, addr); return; }

    last_seq = seq;
    have_seq = true;

    /* ACK with the page address and sequence so the host can track. */
    emit_header();
    bl_uart_write_str(BL_KW_ACK);
    bl_uart_write(' ');
    bl_uart_write_str(BL_KW_DATA);
    bl_uart_write(' ');
    bl_uart_write_hex16(addr);
    bl_uart_write(' ');
    bl_uart_write_hex8(seq);
    emit_trailer();
}

static void handle_blverify(const char *p, const char *end)
{
    uint16_t host_crc;
    if (!expect_space(&p, end) || !parse_hex_u16(&p, &host_crc)) {
        emit_nak('P', false, 0); return;
    }

    uint16_t actual = bl_crc16_flash_range((uint16_t)BL_APP_START, (uint16_t)BL_APP_CRC_ADDR);
    if (actual != host_crc) {
        emit_nak('V', true, actual); return;
    }

    /* Commit: write the trailer so the next reset boots the app. */
    bl_nvm_status_t st = bl_nvm_write_crc_trailer(host_crc);
    if (st != BL_NVM_OK) { emit_nak('E', true, (uint16_t)BL_APP_CRC_ADDR); return; }

    emit_header();
    bl_uart_write_str(BL_KW_ACK);
    bl_uart_write(' ');
    bl_uart_write_str(BL_KW_VERIFY);
    bl_uart_write(' ');
    bl_uart_write_hex16(host_crc);
    emit_trailer();
}

static void handle_blreset(void)
{
    emit_ack_simple(BL_KW_RESET);
    reset_req = true;
}

/* -------------------- frame parser / dispatcher -------------------- */

static void dispatch_frame(void)
{
    /* Buffer holds the entire frame including the trailing \r at rx_buf[rx_len-1]. */
    const char *p   = rx_buf;
    const char *end = rx_buf + rx_len;

    if (p == end || *p++ != BL_STARTCHAR) return;

    bool broadcast = false;
    if (p < end && *p == BL_STARTCHAR) {
#ifdef BL_ACCEPT_BROADCAST
        broadcast = true;
        host_addr = 0;
        p++;
#else
        return;
#endif
    } else {
        uint8_t from, to;
        if (!parse_hex_u8(&p, &from) || !parse_hex_u8(&p, &to)) return;
        if (to != (uint8_t)BL_DCN_ADDRESS) return;
        if (p >= end || *p++ != BL_SEPARATOR) return;
        host_addr = from;
    }
    (void)broadcast;

    /* Extract keyword: from p until space or ':' or end. */
    const char *kw = p;
    while (p < end && *p != ' ' && *p != BL_SEPARATOR && *p != BL_ENDCHAR) p++;
    uint16_t kwlen = (uint16_t)(p - kw);
    if (kwlen == 0) return;

    /* Payload spans from p to the next ':' (the trailer separator). */
    const char *payload     = p;
    const char *payload_end = p;
    while (payload_end < end && *payload_end != BL_SEPARATOR && *payload_end != BL_ENDCHAR) {
        payload_end++;
    }

    if      (keyword_equals(kw, kwlen, BL_KW_INFO))   handle_blinfo();
    else if (keyword_equals(kw, kwlen, BL_KW_ERASE))  handle_blerase(payload, payload_end);
    else if (keyword_equals(kw, kwlen, BL_KW_DATA))   handle_bldata(payload, payload_end);
    else if (keyword_equals(kw, kwlen, BL_KW_VERIFY)) handle_blverify(payload, payload_end);
    else if (keyword_equals(kw, kwlen, BL_KW_RESET))  handle_blreset();
    /* Unknown keyword: silently ignored, matching the application's behaviour. */
}

bool bl_dcn_service(void)
{
    bool processed = false;

    while (bl_uart_rx_ready()) {
        uint8_t c = bl_uart_read();

        if (rx_state == RX_IDLE) {
            if (c != (uint8_t)BL_STARTCHAR) continue;
            rx_state = RX_COLLECTING;
            rx_len = 0;
        }

        if (rx_len >= BL_RX_BUF_SIZE) {
            /* Overflow — discard this frame, return to idle and resync on next '/'. */
            rx_state = RX_IDLE;
            rx_len = 0;
            continue;
        }

        rx_buf[rx_len++] = (char)c;

        if (c == (uint8_t)BL_ENDCHAR) {
            dispatch_frame();
            rx_state = RX_IDLE;
            rx_len = 0;
            processed = true;
            /* Return after one frame so callers can interleave other work. */
            break;
        }
    }

    return processed;
}

bool bl_dcn_reset_requested(void)
{
    return reset_req;
}
