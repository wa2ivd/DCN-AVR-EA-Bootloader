# DCN-AVR-EA-Bootloader

Serial bootloader for AVR EA-series microcontrollers (AVR64EA28/32/48 and
relatives), riding on the Device Control Network **(DCN)** protocol.
This Bootloader was initially developed for the SPS-1 Smart Switch by A2Z Tech LLC.
The plan is to include this or a compatible Bootloader for any future A2Z products designed to
work in the DCN / Station Controller eco-system.

## What is DCN
The Device Control Network protocol was developed by George Zafiropoulos, KJ6VU of Sierra Radio Systems and Packtenna
(https://www.sierraradio.net/) and used by SRS Station Controller Products.  It is a simple, human readable, serial protocol
intended use on half-duplex communication networks such as RS-485 or LoRa data radios.

## Bootloader overview
The bootload protocol (see PROTOCOL.md in docs) is designed to be compatible with DCN.  As such, a device could theoretically
be updated while connected to a DCN network with other devices.  Other than a short period of extremely heavy traffic from the
program data, the network should be unaffected.
However, it is envisioned that devices that need to be updated will typically be directly connected to a USB or RS-485 serial
port, stand-alone, on a PC for program updates.
In this implementation, the device DCN address when the Bootloader is active is fixed at "BB".  This would limit in-network updates
to 1 device at a time.  This likely will not present any "real" restrictions as it is unlikely that there would be a need to update
multiple devices on a network simultaneously.
NOTE:  A DCN address of "BB" doesn't strictly follow the published protocol as the DCN documentation states that valid addresses
are from 00-99.

The intent is for the bootloader to be common across all Microchip AVRxxEA series 
microcontrollers used in this product line.  Product unique settings, like bootloader entry mechanism settings live in one small header.

This project is written specifically for Microchip AVR EA series microcontrollers.  However, the code and protocol could easily be
ported to other processors and languages as needed.

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
- On products with a USB serial port (SPS-1 hardware with the USB-to-serial
  bridge), automatically selects between the RS-485 bus and USB as the host
  port at startup — see below.


## USB serial support

Products can define a second, USB-facing UART in their config
(`BL_USART_USB` + `BL_USB_PRESENT()`). On the SPS-1 this is a USB-to-serial
bridge on USART2 (PF0 = TX to bridge, PF1 = RX from bridge); the bridge's
USBCFG status output on PA1 reads high once a USB host has enumerated it.

Port selection is automatic:

After deciding to stay in the bootloader, the firmware waits 500 ms for power and port conditions to stabilize.  In the SPS-1,
port PA1 is tied to 3.3V power from the USB port.  If this pin his high, then USB port power is being provided and the assumption is
that we are connected to a USB port and will use the USB-UART bridge port for communication.  If it is low, then the RS-485 UART is used.
A more definitive test would be to check the bridge chip to see if the USB port has been enumarated.  This is provided by a USBCFG pin
on the chip used in the SPS-1.  However, this pin proved to be unreliable in testing.  So USB power is being used instead.

The "ignored" port is simply never enabled, so it can neither receive nor
transmit. Baud rate, framing (8N1), protocol, and host tooling are identical
on both ports — `bl_flash.py` doesn't know or care which one it's talking
through. The choice is latched for the session: plugging or unplugging USB
takes effect on the next reset (power cycle or `BLRESET`).

A normal boot into the application is **not** delayed — the 500 ms settle
wait only happens once the bootloader has already decided to stay resident.

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

## Quick start

There are no Windows or other OS Packages for this repository.  The code must be
built for the specific AVR (or other controller) chip that you intend to use it in.

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

**Linux / macOS:**
NOTE:  I do not have a MAC and I have not tested or built this in Linux.  These instructions
are approximate and furnished AS-IS.

put `avr-gcc` and the DFP somewhere the compiler can find
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
bootstrap path. Once flashed, all future application updates go through the bootloader.
There is currently no provision to update the bootloader via the bootloader.
Bootloader updates must be done via a UPDI programmer.

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
# 2. Power-cycle. (If updating over USB, have the cable plugged in before
#    power-up — the port choice is sampled 500 ms after entry.)
# 3. Run the updater, pointing --port at either the RS-485 adapter or the
#    device's own USB serial port:
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
