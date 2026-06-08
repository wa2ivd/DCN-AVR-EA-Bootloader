# DCN-AVR-EA-Bootloader

Serial bootloader for AVR EA-series microcontrollers (AVR64EA28/32/48 and
relatives), riding on the existing Device Control Network **(DCN)** protocol
developed by Sierra Radio Systems  (https://www.sierraradio.net/)
This protocol is supported by the Smart Power Switch (SPS-1) and will be supported
by future A2Z Tech products. 
The intent is for the bootloader to be common across all Microchip AVRxxEA series 
microcontrollers.  Product unique settings, like bootloader entry mechanism settings
live in one small header.

## What it does

- Lives in the BOOT section of flash (4 KB by default, fuse-configured).
- Runs first on every reset.
- Decides between three paths:
  1. **Hardware trigger asserted** (e.g. SPS-1 BCD address switch set to 0xF)
     → enter update mode.
  2. **Application image invalid** (CRC trailer mismatch) → enter recovery
     mode (same protocol, just no operator action).
  3. **Otherwise** → jump to the application at `BL_APP_START` (`0x1000` for SPS-1).
- In update / recovery mode, speaks a tiny set of DCN-framed commands over
  the same UART the application uses. The host (`tools/bl_flash.py`) drives
  erase → page-by-page write → verify → reset.

The application itself contains **no** bootloader-entry code. The only way
into the bootloader is the hardware switch or a bad image — which means a
field unit can't be bricked by a runaway app entering the bootloader by
mistake.

## Repository layout

```
DCN-AVR-EA-Bootloader/
├── Makefile                    avr-gcc build (make PRODUCT=SPS-1)
├── build.sh                    Windows wrapper that finds MPLAB tools + DFP
├── src/                        portable bootloader sources
│   ├── bl_main.c               entry, decision logic, main loop
│   ├── bl_uart.c / .h          minimal polled USART driver
│   ├── bl_dcn.c / .h           DCN frame parser + responder
│   ├── bl_nvm.c / .h           page erase / page write helpers
│   ├── bl_crc.c / .h           CRC-16/CCITT-FALSE
│   ├── bl_fuses.c              FUSES = { ... } block
│   ├── bl_config.h             pulls in the active product config
│   └── bl_protocol.h           wire protocol definitions
├── products/                   one config header per product
│   ├── SPS-1/bl_product_config.h
│   └── template/bl_product_config.h
├── tools/
│   ├── patch_app_crc.py        post-link CRC trailer stamper for app hex
│   └── bl_flash.py             host-side firmware updater (pyserial)
├── app_support/                docs + linker fragment for the app side
└── docs/
    ├── DESIGN.md               architectural rationale
    └── PROTOCOL.md             wire-level protocol reference
```

## Quick start (SPS-1)

### Building the bootloader

Requires `avr-gcc` (or XC8 for AVR, which is avr-gcc underneath) with the
AVR EA device pack installed, plus GNU make and a Unix-ish shell.

**Windows (MPLAB X + XC8 installed):** use the build wrapper. It auto-locates
the latest XC8, MPLAB X, and AVR-Ex DFP installs under `C:\Program Files\`
and forwards everything else to `make`.

```sh
./build.sh PRODUCT=SPS-1            # full build
./build.sh PRODUCT=SPS-1 clean      # any make target works
./build.sh DFP=/custom/path PRODUCT=SPS-1   # override the auto-discovered DFP
```

Run it from git-bash or MSYS2 — the Makefile uses `rm -rf` / `mkdir -p`, so a
bash environment is required either way. Toolchain locations the wrapper checks
are documented in the header of [build.sh](build.sh).

**Linux / macOS:** put `avr-gcc` and the DFP somewhere the compiler can find
them and call `make` directly (or `./build.sh`, which becomes a thin pass-through
on non-Windows hosts).

```sh
make PRODUCT=SPS-1
```

Produces:
- `build/SPS-1/SPS-1-bootloader.hex` — single Intel-hex with both flash
  contents (BOOT section) and fuse bytes (at LMA 0x820000+, the same
  convention MPLAB uses). Flash this one file and a tool that dispatches
  by address — MPLAB IPE, Atmel Studio, etc. — programs everything.
- `build/SPS-1/SPS-1-bootloader.lss` — disassembly for debugging
- `build/SPS-1/SPS-1-bootloader.map` — link map

The `size` step enforces that `.text` fits inside the configured BOOT
section (default 2 KB).

### First-time programming via UPDI

The bootloader can only be flashed via UPDI — there is no in-system
bootstrap path. Once flashed, all future updates go through the bootloader.

**MPLAB IPE (or Atmel Studio):** load `build/SPS-1/SPS-1-bootloader.hex`,
hit Program. The hex contains both flash and fuse bytes; IPE programs
each at the right memory based on the ihex address. This is the same
workflow as flashing the SPS-1 application.

**avrdude with a UPDI programmer (PICkit, mEDBG, jtag2updi, etc.):**
avrdude doesn't dispatch by ihex address the way IPE does, so fuses
have to be written explicitly:

```sh
# Fuses (one-time) — values must match those compiled into src/bl_fuses.c.
# OSCCFG = 0x00 selects 20 MHz OSCHF (OSCHFFRQ_20M_gc encodes to 0x00
# per ioavr64ea28.h). SYSCFG0 = 0xD0 disables RSTPIN, keeps UPDI, CRC16.
# SYSCFG1 = 0x07 → 64 ms startup. WDTCFG = 0x00 → WDT disabled.
avrdude -c <programmer> -p avr64ea28 \
    -U bootsize:w:0x10:m -U codesize:w:0x00:m \
    -U wdtcfg:w:0x00:m   -U osccfg:w:0x00:m \
    -U syscfg0:w:0xD0:m  -U syscfg1:w:0x07:m

# Bootloader code (the flash region of the combined hex; avrdude
# silently ignores the fuse-region records when memtype is `flash`).
avrdude -c <programmer> -p avr64ea28 \
    -U flash:w:build/SPS-1/SPS-1-bootloader.hex:i
```

### Building and flashing the application

The application's build must be modified to:
- Start `.text` at `0x0800` (so its reset vector lands at the start of
  APPCODE, where IVSEL=0 expects interrupt vectors).
- Drop its own `FUSES = { ... }` block (the bootloader owns the fuses).
- Run `tools/patch_app_crc.py` after link to stamp the CRC trailer.

See [app_support/README.md](app_support/README.md) for the details.

Then flash via the bootloader:

```sh
# 1. Set the device's BCD switch to position F.
# 2. Power-cycle.
# 3. Run the updater:
python tools/bl_flash.py --port COM5 --hex build/SPS-1.crc.hex
# 4. When it finishes, set the switch back to the normal address.
```

## Adding a new product

```sh
cp -r products/template products/NewProduct
$EDITOR products/NewProduct/bl_product_config.h
make PRODUCT=NewProduct
```

The product config must define MCU-specific UART, clock, port-init, and
trigger-detection macros. See the template for the full list.

See `docs/DESIGN.md` for the full rationale and a list of carrying-over
open questions.
