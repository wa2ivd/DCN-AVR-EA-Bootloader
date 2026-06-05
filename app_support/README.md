# Application-side changes for bootloader integration

To make an existing application work with this bootloader, three things
must change in the application build:

1. **`.text` start moves from `0x0000` to the APPCODE base
   (`BL_APP_START`, default `0x1000`).**
2. **The application stops setting fuses.** The bootloader owns the
   fuses; the app's `FUSES = { ... }` block must be removed.
3. **A post-link step stamps the CRC trailer** into the hex file at
   `BL_APP_CRC_ADDR` (default `0xFFFE`).

The interrupt vector table needs no special handling — avr-gcc emits
it at the start of `.text` automatically, and with `CPUINT.CTRLA.IVSEL`
left at its default of 0 the CPU looks for vectors at the start of
APPCODE, which is exactly where they'll be.

## 1. Relocate `.text`

### MPLAB X (XC8 for AVR)

In project properties → XC8 Linker → Additional options, add:

```
-Wl,--section-start=.text=0x1000
```

### Plain avr-gcc / Makefile

Add the same flag to your `LDFLAGS`:

```make
LDFLAGS += -Wl,--section-start=.text=0x1000
```

`app_section.ld_fragment` in this directory is a snippet you can paste
into a custom linker script if you have one — but the command-line flag
above is the easier approach for most projects.

After relinking, inspect the map file: the reset vector should be at
`0x1000` and the application's `.data` LMA should follow `.text` and
fit inside the APPCODE region. The total `.text + .data` size must be
less than `BL_FLASH_END − BL_APP_START − 2` (the `−2` reserves room for
the CRC trailer).

## 2. Remove the application's `FUSES = { ... }` block

In an MCC-generated project this lives in `mcc_generated_files/system/src/config_bits.c`.
Delete the file (or comment out its `FUSES = { ... }` definition) and
remove it from the build.

If the app references any of those fuse macros at runtime, factor them
into a separate file. The fuses themselves are written once at the
factory by the bootloader's `*.fuse.hex`, never touched again.

The application is still free to configure runtime peripherals
(WDT.CTRLA, CLKCTRL.MCLKCTRLA, etc.) at runtime — those aren't fuses.

## 3. Run `patch_app_crc.py` after link

Add a post-link step to your build:

```sh
python ../DCN-AVR-EA-Bootloader/tools/patch_app_crc.py \
    --in  build/MyApp.hex \
    --out build/MyApp.crc.hex \
    --app-start 0x1000 \
    --crc-addr  0xFFFE
```

The output `MyApp.crc.hex` is what you flash via the bootloader's
host updater:

```sh
python ../DCN-AVR-EA-Bootloader/tools/bl_flash.py \
    --port COM5 --hex build/MyApp.crc.hex
```

In MPLAB X you can add this as a post-build step under project
properties → Building.

## Sanity check before first flash

After making the three changes, inspect the produced hex with any hex
viewer:

- Address `0x1000` should contain the first 4 bytes of a `jmp` (the
  application's reset vector).
- Address `0xFFFE..0xFFFF` should contain the CRC trailer (added by
  the patch step, two bytes little-endian).
- No data should appear below `0x1000`.

If those three hold, the bootloader will recognise the image and boot
it on the first reset after a successful BLVERIFY.
