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

One additional gotcha for MCC-generated applications: any peripheral
initialization that writes to a CCP-protected register needs the proper
unlock sequence — MCC sometimes emits bare assignments that silently
fail. See [section 4](#4-fix-mcc-emitted-ccp-protected-register-writes)
at the bottom.

## 1. Relocate `.text`

### MPLAB X 6.x (legacy NetBeans-style Makefile build)

In project properties → XC8 Linker → Additional options →
**Extra LD options**, add:

```
-Wl,--defsym=__TEXT_REGION_ORIGIN__=0x1000
```

This redefines the `text` memory region's origin in the avr-libc
linker script (`avrxmega2.xn` and friends) at link time. The relocation
applies to `.text`, the vector table, and everything downstream.

> ⚠️ **Don't** use `-Wl,--section-start=.text=0x1000`. The XC8 v3 linker
> wrapper rejects it with "Could not allocate section '.text' at 0x1000"
> per-object, even though `.text` would otherwise fit. The `defsym`
> approach above works under both XC8 and plain avr-gcc.

### VSCode + Microchip MPLAB extensions (CMake / Ninja build)

The MPLAB extension pack uses CMake under `cmake/<project>/<conf>/`,
not the NetBeans Makefile. The generated `CMakeLists.txt` and
`.generated/` files are regenerated on every project-config refresh, so
add the flag in the hand-editable `user.cmake` instead:

```cmake
# user.cmake — preserved across MPLAB regenerations.

file(READ "${CMAKE_CURRENT_LIST_DIR}/.generated/main.cmake" _main)
string(REGEX MATCH "add_executable\\(([A-Za-z0-9_]+)" _ "${_main}")
set(_image_target "${CMAKE_MATCH_1}")

target_link_options(${_image_target} PRIVATE
    "-Wl,--defsym=__TEXT_REGION_ORIGIN__=0x1000")
```

The image target name has a random hash suffix that MPLAB regenerates,
so we discover it from `main.cmake` at configure time. The same
`user.cmake` is also the right place to hook the post-link CRC stamper
(see [section 3](#3-run-patch_app_crcpy-after-link)).

### Plain avr-gcc / Makefile

Either flag works:

```make
LDFLAGS += -Wl,--defsym=__TEXT_REGION_ORIGIN__=0x1000
# or, equivalently, on plain avr-gcc (but NOT XC8):
LDFLAGS += -Wl,--section-start=.text=0x1000
```

`app_section.ld_fragment` in this directory is a snippet you can paste
into a custom linker script if you have one — but a command-line flag
is the easier approach for most projects.

### After relinking — verify

Inspect the map file. The reset vector should be at `0x1000`, and the
application's `.data` LMA should follow `.text` and fit inside the
APPCODE region. The total `.text + .data` size must be less than
`BL_FLASH_END − BL_APP_START − 2` (the `−2` reserves room for the CRC
trailer).

## 2. Remove the application's `FUSES = { ... }` block

In an MCC-generated project this lives in
`mcc_generated_files/system/src/config_bits.c`. Delete the file (or
delete just the `FUSES = { ... }` definition) and remove it from the
build.

If you leave it in place, the linker will emit a `.fuse` section
alongside the bootloader's, and the resulting hex either won't flash
through `bl_flash.py` (which only writes APPCODE) or will silently
change fuse values if UPDI-flashed without preserving the bootloader.

If the app references any of those fuse macros at runtime, factor them
into a separate file. The fuses themselves are written once at the
factory by the bootloader's `*.fuse.hex`, never touched again.

The application is still free to configure runtime peripherals
(WDT.CTRLA, CLKCTRL.MCLKCTRLA, etc.) at runtime — those aren't fuses.
But see section 4 for a related gotcha.

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

### MPLAB X 6.x (legacy)

Add the python call as a post-build step under project properties →
Building → "Execute this line after build". The variable
`${ImagePath}` expands to the just-built hex.

### VSCode + MPLAB extensions (CMake)

Add to the same `user.cmake` from section 1:

```cmake
# Resolve the bootloader repo location. Override via -DBL_REPO_DIR=...
# if your checkout layout differs from the default sibling-directory
# convention.
if(NOT DEFINED BL_REPO_DIR)
    set(BL_REPO_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../../DCN-AVR-EA-Bootloader")
endif()

add_custom_command(
    TARGET ${_image_target} POST_BUILD
    COMMAND python
            "${BL_REPO_DIR}/tools/patch_app_crc.py"
            --in  "${output_dir}/${image_base_name}.hex"
            --out "${output_dir}/${image_base_name}_BL.hex"
            --app-start 0x1000
            --crc-addr  0xFFFE
    COMMENT "Stamping CRC-16/CCITT-FALSE trailer for bootloader"
)
```

(Replace `${output_dir}` and `${image_base_name}` with whatever
variables your project's `main.cmake` defines — they vary by MPLAB
extension version.)

### Note on hex content

The patched hex may contain extended-linear-address records pointing
to non-flash regions: EEPROM defaults at `0x810000+`, USERROW at
`0x850000+`, etc. These are preserved by the patcher in the output
hex, but `bl_flash.py` filters them out — the bootloader currently
only writes APPCODE flash. The records being present is harmless and
forward-compatible: if a future bootloader extension adds EEPROM
write support, the hex won't need to change.

## 4. Fix MCC-emitted CCP-protected register writes

This one bites if you use MCC to generate peripheral drivers for the
WDT, CPUINT, NVMCTRL, or clock unit. MCC sometimes emits bare register
assignments to **CCP-protected** registers, which silently fail on the
AVR EA — no exception, no warning, no error code, the register just
doesn't change.

The most common offender is `WDT_Initialize()` in
`mcc_generated_files/peripheral/src/wdt.c`. MCC commonly emits:

```c
int8_t WDT_Initialize(void) {
    WDT.CTRLA = 0x78;   // silently no-op'd — CCP key not written
    WDT.STATUS = 0x80;  // same
    return 0;
}
```

This works **without** the bootloader because the WDTCFG fuse is
typically set to a running-WDT value at reset (so the bare assignments
are no-ops that don't matter — the fuse already did the work). It
breaks **with** the bootloader because:

- The bootloader's default `BL_FUSE_WDTCFG` is `0x00` (WDT off at
  reset). Products can override this in their `bl_product_config.h`,
  but most don't.
- The bootloader's `bl_main.c` explicitly calls
  `_PROTECTED_WRITE(WDT.CTRLA, 0)` before jumping to the application,
  which would disable the WDT even if the fuse had enabled it.

Net effect: the application's `WDT_Initialize()` runs, all its writes
silently fail, the WDT stays off for the entire life of the app.

**Fix:** wrap each CCP-protected write in `_PROTECTED_WRITE()`:

```c
int8_t WDT_Initialize(void) {
    _PROTECTED_WRITE(WDT.CTRLA, 0x78);
    _PROTECTED_WRITE(WDT.STATUS, 0x80);
    return 0;
}
```

`_PROTECTED_WRITE` is the avr-libc macro that writes `0xD8` to `CCP`
then the value to the target register within the required 4-instruction
window.

CCP-protected registers on the AVR EA include: `WDT.CTRLA`,
`WDT.STATUS` (LOCK bit), `CPUINT.CTRLA` (IVSEL, CVT, LVL0RR),
`CPUINT.LVL0PRI`, `CPUINT.LVL1VEC`, `CLKCTRL.MCLKCTRLA`,
`CLKCTRL.MCLKCTRLB`, `CLKCTRL.MCLKLOCK`, `CLKCTRL.XOSCHFCTRLA`, and
`NVMCTRL.CTRLA` (with a different SPM key — use `_PROTECTED_WRITE_SPM`
for that one).

Scan any MCC-generated `_Initialize` functions in your project for
bare assignments to these and wrap them. The CPU manual lists the
full set under "Configuration Change Protection" in each peripheral's
register table.

> ⚠️ **MCC will re-emit the bare writes if it regenerates the file.**
> Re-apply the wraps after any MCC peripheral-config change. Keep a
> note in the project README listing the hand-edited files so it
> doesn't get lost.

## Sanity check before first flash

After making the changes, inspect the produced hex with any hex viewer:

- Address `0x1000` should contain the first 4 bytes of a `jmp` (the
  application's reset vector).
- Address `0xFFFE..0xFFFF` should contain the CRC trailer (added by
  the patch step, two bytes little-endian).
- No data should appear below `0x1000` in the lower-64K segment.
  (Data records under an extended-linear-address record pointing to
  `0x810000+`, `0x850000+`, etc., are fine — those are EEPROM /
  USERROW defaults and the bootloader filters them out.)

If those three hold, the bootloader will recognise the image and boot
it on the first reset after a successful BLVERIFY.
