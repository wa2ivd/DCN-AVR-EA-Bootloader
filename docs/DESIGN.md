# Design notes

This document captures the *why* behind the choices in the bootloader.
For the *what*, see the source files and `PROTOCOL.md`.

## Memory layout

AVR EA flash is split via two 8-bit fuses, both counted in 256-byte
units (datasheet §8.8.2.7):

```
0x0000              0x0FFF 0x1000                          0xFFFF
+--------------------+      +-------------------------------+
| BOOT (4 KB)        |      | APPCODE (60 KB)               |
|  bootloader code   |      |  application code             |
|  reset vector      |      |  app's own vectors at start   |
+--------------------+      +-------------------------------+
       BOOTSIZE = 0x10              CODESIZE = 0x00
```

`CODESIZE=0` means "APPCODE fills flash from BOOTEND+1 to FLASHEND, no
APPDATA section" (datasheet §11.3.1.1.2). We pick that to keep the app
as large as possible — the SPS-1 currently builds to ~14 KB so there's
plenty of headroom.

The last two bytes of APPCODE hold the CRC trailer:

```
0xFFFE: CRC-16 low byte
0xFFFF: CRC-16 high byte
```

`BL_APP_CRC_ADDR` in `bl_product_config.h` controls the position.

## Boot decision

```
       reset (PC = 0x0000, always, regardless of IVSEL)
              │
              ▼
       disable WDT, init clock + NVM + GPIO
              │
              ▼
       hardware trigger asserted? ──► yes ─► UPDATE MODE
              │ no
              ▼
       app CRC valid? ──► no  ──► RECOVERY MODE (same protocol)
              │ yes
              ▼
       jump_to_app() — JMP 0x0800
```

The trigger source is product-specific (`BL_TRIGGER_ASSERTED()`). On
SPS-1 it reads the DCN address BCD switch and matches against position
0xF. On a board without a switch you can wire it to a jumper, a button
held during power-up, an EEPROM byte set out-of-band — anything you can
turn into a boolean expression.

There is **no** in-band command to enter the bootloader. This is
deliberate: the application has no knowledge of the bootloader's
existence and cannot be tricked into a forced update by a bus message.

## Why no vector trampolines

The original plan called for a vector forwarding table because we
assumed AVR EA, like older AVRs, vectored interrupts to a fixed
address. The datasheet confirms it has `CPUINT.CTRLA.IVSEL` — same as
the megaAVR 0 / AVR Dx family (§15.3.2.2). When `IVSEL = 0` (the
default), interrupt vectors live at the start of APPCODE.

The bootloader sets `IVSEL = 0` (or leaves the default) and disables
interrupts globally. The application, linked to start at `0x1000`,
gets its own vector table at the start of APPCODE — exactly where
the CPU expects it. No trampolines, no forwarders, no runtime
indirection cost. Just two normal AVR binaries that happen to live
in different parts of flash.

The reset vector at `0x0000` is always in BOOT regardless of IVSEL
(§15.5.1, note 1 on IVSEL), which is why the bootloader runs first.

## NVMCTRL programming

`bl_nvm.c` uses the SPM-instruction path with the Z-pointer holding
the byte address. Single-instruction `spm` writes one word into the
page buffer at the byte slot determined by Z (datasheet §11.3.2).

For each page write the sequence is:

```
1. Wait until NVMCTRL.STATUS.FLBUSY == 0
2. Issue NOCMD via CCP-protected write (clears any prior command)
3. For each word i in the page:
     R0:R1 ← data word
     Z     ← addr + i
     spm
4. Issue FLPW via CCP-protected write
5. Wait until FLBUSY == 0
6. Read NVMCTRL.STATUS for any ERROR code
```

Erase is the same except for one dummy `spm` (to latch the address)
followed by `FLPER`.

We use the separate `FLPER`/`FLPW` commands rather than the combined
`FLPERW` because the host-side protocol uses BLERASE as a discrete
step. This gives the host the freedom to erase a range up front and
then push pages one at a time without per-page erase latency.

Per §11.3.1.1.3, **only code executing from the BOOT section may write
APPCODE**. That's the entire reason for placing the bootloader at
`0x0000` and the app at `0x0800` instead of the other way around.

## CRC choice

CRC-16/CCITT-FALSE (`poly=0x1021, init=0xFFFF, refin=false,
refout=false, xorout=0x0000`). Reasons:

- The AVR EA has a CRCSCAN peripheral with a CCITT-16 polynomial
  (§8.8.2.4, SYSCFG0.CRCSEL) so even though we use a software
  implementation right now, switching to the hardware peripheral
  later requires no protocol change.
- 16 bits matches the natural word size of the chip and fits two
  bytes — convenient for the trailer.
- Per-page CRC catches isolated bit errors on the bus; whole-image
  CRC catches anything the per-page CRCs missed and confirms the
  host's view of the image matches what landed in flash.

The "erased trailer reads 0xFFFF" case is handled specially: the
bootloader treats `0xFFFF` as **always invalid** even if a real CRC
happens to compute to `0xFFFF`. This keeps "freshly erased flash" and
"valid image" unambiguous.

## Power-loss safety

The trailer at `BL_APP_CRC_ADDR` is written **only** after BLVERIFY
succeeds. If power drops at any point during a flash update:

- Before BLVERIFY → trailer is still 0xFFFF → next reset enters
  recovery mode → host can retry.
- During BLVERIFY's trailer write → the trailer page is partially
  programmed → on next reset the computed CRC won't match the
  partially-written trailer → recovery mode → retry.
- After BLVERIFY's trailer write completes → success, app boots.

The bootloader itself lives in BOOT and is never written by the
bootloader, so it can never corrupt itself.

## Generic / per-product split

Anything that touches the bus or the board is in
`products/<name>/bl_product_config.h`:

- UART instance, CTRLA / CTRLB values, baud
- Optional second UART for a USB-to-serial bridge (`BL_USART_USB` +
  `BL_USB_PRESENT()`): the bridge's "configured by host" pin is sampled
  once after a settle delay and the whole session runs on that port,
  with the other USART never enabled
- F_CPU and clock-init macro
- Port-init macro (UART pins, trigger pull-ups, etc.)
- Trigger-detection expression
- Memory layout constants (mostly the same across AVR EA parts)
- DCN bootloader address
- Whether broadcast is accepted

Everything else is portable. A new product is one new header.

## Carrying-over open items

1. ~~**Inline `jmp` constant encoding**~~ — *Resolved (SPS-1 build,
   avr-gcc via XC8 v3.10):* `asm volatile("jmp %0" :: "i"(BL_APP_START))`
   emits `0c 94 00 08` — opcode word `0x940c` + word-address `0x0800`,
   i.e. `jmp 0x1000`. Matches `BL_APP_START`. No assembler-side surprise
   in converting the byte-address operand to the word-address encoding.
2. **WDT lock behaviour** — if a previous app fuse-set a locked
   WDT, the bootloader's `_PROTECTED_WRITE(WDT.CTRLA, 0)` is a
   no-op. Programming sequences finish in <10 ms, well inside any
   reasonable WDT window, so this is unlikely to matter in
   practice but worth confirming.
3. ~~**F_CPU vs. fuse OSCCFG**~~ — *Resolved (cross-checked against the
   SPS-1 application's MPLAB-generated `config_bits.c`):* both
   bootloader and application emit `OSCCFG = OSCHFFRQ_20M_gc`, which
   per ioavr64ea28.h encodes to byte `0x00`. The OSCHF reset default
   on AVR EA is already 20 MHz with that fuse, and `BL_CLOCK_INIT()`
   divides by 2 → 10 MHz CPU = `F_CPU=10000000UL`. A future port to a
   product with a different `OSCCFG` should keep the relationship
   `(OSCHF_MHz / BL_CPU_DIV) * 1e6 == F_CPU` consistent in the
   product config + Makefile pair.
4. **Verify-CRC range** — we exclude the trailer bytes themselves
   from the CRC computation. The host-side `patch_app_crc.py`
   matches this convention. If the convention ever changes, both
   sides must change together.
