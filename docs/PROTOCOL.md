# DCN-AVR-EA-Bootloader Wire Protocol

The bootloader speaks a small subset of the DCN protocol used by the
application firmware, so they can share an RS-485 / single-wire bus
without conflict.

## Framing

All frames use the existing DCN envelope:

```
/FFTT:KEYWORD[ payload...]:XX\r
```

| Element  | Width        | Description                                       |
|----------|--------------|---------------------------------------------------|
| `/`      | 1 byte (0x2F)| Start-of-frame                                    |
| `FF`     | 2 hex digits | Sender address (host conventionally `00`)         |
| `TT`     | 2 hex digits | Target address (bootloader = `BL_DCN_ADDRESS`)    |
| `:`      | 1 byte (0x3A)| Separator                                         |
| `KEYWORD`| ASCII        | Opcode (`BLINFO`, `BLERASE`, …)                   |
| payload  | varies       | Opcode-specific, space-separated fields           |
| `:`      | 1 byte (0x3A)| Separator before the check field                  |
| `XX`     | 2 bytes      | Placeholder check field (same as app protocol)    |
| `\r`     | 1 byte (0x0D)| End-of-frame                                      |

Broadcast frames (`//KEYWORD...:XX\r`) are accepted only if the product
config defines `BL_ACCEPT_BROADCAST` (SPS-1 does).

### Why no commas

Bootloader payloads use **spaces** to separate fields. The application's
DCN parser tokenises the keyword by reading up to the first `,` or `:`,
so a space-separated bootloader payload looks like one giant unknown
keyword to it — which it silently discards. This lets bootloader traffic
flow on a bus shared with running application devices without confusing
them.

## Commands

### `BLINFO` — handshake / discovery

```
Request:   /00BB:BLINFO:XX\r
Response:  /BB00:BLACK BLINFO DDDDDD PPPP SSSS EEEE TTTT VVVV C:XX\r
```

| Field    | Width | Description                                   |
|----------|-------|-----------------------------------------------|
| `DDDDDD` | 6 hex | `SIGROW.DEVICEID[0..2]` (e.g. `1E9620`)       |
| `PPPP`   | 4 hex | Flash page size in bytes (e.g. `0080` = 128)  |
| `SSSS`   | 4 hex | `BL_APP_START`                                |
| `EEEE`   | 4 hex | `BL_FLASH_END`                                |
| `TTTT`   | 4 hex | `BL_APP_CRC_ADDR`                             |
| `VVVV`   | 4 hex | Bootloader version (`BL_VERSION`)             |
| `C`      | 1     | `1` if current app CRC valid, `0` if not      |

A host that doesn't get a `BLINFO` response within ~2 s should assume the
device is not in bootloader mode (operator forgot the switch, etc.).

### `BLERASE` — erase a flash range

```
Request:   /00BB:BLERASE SSSS EEEE:XX\r
Response:  /BB00:BLACK BLERASE:XX\r          (success)
           /BB00:BLNAK R SSSS:XX\r           (error)
```

`SSSS` and `EEEE` are inclusive byte addresses that must:
- Both lie inside `[BL_APP_START, BL_FLASH_END]`.
- Be page-aligned (`SSSS` at page start, `EEEE+1` at page boundary).

`BLERASE` resets the BLDATA sequence-number tracker, so the next BLDATA
may use any sequence value.

### `BLDATA` — write one flash page

```
Request:   /00BB:BLDATA AAAA SS <2*PAGE hex bytes> CCCC:XX\r
Response:  /BB00:BLACK BLDATA AAAA SS:XX\r   (success)
           /BB00:BLNAK R AAAA:XX\r           (error)
```

| Field    | Width        | Description                                |
|----------|--------------|--------------------------------------------|
| `AAAA`   | 4 hex        | Page byte address (page-aligned)           |
| `SS`     | 2 hex        | 8-bit sequence number, wraps at 0xFF       |
| data     | 2 × pagesize | Page bytes, hex-encoded, no separators     |
| `CCCC`   | 4 hex        | CRC-16/CCITT-FALSE of the raw page bytes   |

After the first BLDATA in an erase epoch, subsequent BLDATA frames must
have sequence `(last+1) & 0xFF`.

### `BLVERIFY` — verify and commit

```
Request:   /00BB:BLVERIFY CCCC:XX\r
Response:  /BB00:BLACK BLVERIFY CCCC:XX\r    (success — trailer committed)
           /BB00:BLNAK V XXXX:XX\r           (mismatch; XXXX is what BL computed)
```

The host's `CCCC` is the CRC-16/CCITT-FALSE over the byte range
`[BL_APP_START, BL_APP_CRC_ADDR)`. The bootloader recomputes the same
CRC from flash. On match it writes `CCCC` to `BL_APP_CRC_ADDR` (the
trailer), which is what subsequent resets check before jumping to the
app.

### `BLRESET` — finish

```
Request:   /00BB:BLRESET:XX\r
Response:  /BB00:BLACK BLRESET:XX\r
```

After the ACK is flushed onto the wire, the bootloader configures the
WDT for the shortest available period and spins until it resets. The
operator should already have moved the BCD switch back to the device's
normal address before this point.

## NAK reason codes

A single ASCII letter in the field after `BLNAK`:

| Code | Meaning                                                        |
|------|----------------------------------------------------------------|
| `A`  | Address out of range or outside APPCODE                        |
| `B`  | Address not page-aligned                                       |
| `C`  | Payload CRC mismatch                                           |
| `E`  | NVMCTRL hardware error during erase/write                      |
| `L`  | Payload length wrong for declared page                         |
| `P`  | Parse error (bad hex digit, malformed field)                   |
| `S`  | Unexpected sequence number                                     |
| `V`  | Whole-image verify CRC mismatch                                |

The NAK frame's second field is the relevant address (page address for
BLDATA, range start for BLERASE, the bootloader's computed CRC for
BLVERIFY).

## Timing notes

At 9600 baud with the SPS-1 page size of 128 bytes:

- BLDATA frame length ≈ 286 bytes → ~300 ms on the wire.
- Page erase + write inside the bootloader ≈ 5–10 ms.
- Full 60 KB application image ≈ 477 pages × 0.31 s ≈ 2.5 minutes.

Bumping the baud rate (e.g. 115200) shortens this proportionally and
requires changing `BL_BAUD` in the product config and recompiling.

## Application-side coexistence

The application's DCN parser silently ignores unknown keywords, so stray
bootloader traffic on the bus does not cause it to misbehave. There is
one transient effect: a `BLDATA` frame is longer than the application's
50-byte receive buffer, so the application will see the buffer fill,
discard the frame mid-stream, and resume on the next `/`. As long as no
operational commands are sent during a firmware update, this is
harmless.
