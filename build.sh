#!/usr/bin/env bash
# DCN-AVR-EA-Bootloader build wrapper.
#
# Why this exists:
#   The Makefile requires avr-gcc, GNU make, and the AVR-Ex device pack (DFP)
#   to be reachable. On a typical Windows dev box those three pieces live in
#   three different directories under "C:\Program Files\Microchip\...", and
#   none of them are on PATH by default. This script finds the latest installed
#   version of each, sets PATH and the DFP variable, then invokes make. On
#   Linux/macOS it just forwards to make — install avr-gcc however you like.
#
# Usage:
#   ./build.sh                          # default target, PRODUCT=SPS-1
#   ./build.sh PRODUCT=SPS-1            # explicit product
#   ./build.sh PRODUCT=SPS-1 clean      # any make target works
#   ./build.sh DFP=/custom/path ...     # override the auto-discovered DFP
#
# Requirements (Windows):
#   - MPLAB X IDE installed (provides make.exe and the AVR-Ex DFP).
#   - MPLAB XC8 for AVR installed (provides avr-gcc and friends).
#   - A bash shell — git-bash or MSYS2 both work. The Makefile uses Unix
#     commands (rm -rf, mkdir -p) so a bash environment is required even
#     without this wrapper.
#
# Requirements (Linux/macOS):
#   - avr-gcc, avr-objcopy, avr-objdump, avr-size, make on PATH.
#   - The AVR-Ex DFP either installed somewhere the compiler can find it or
#     passed explicitly: ./build.sh DFP=/path/to/AVR-Ex_DFP/<ver>

set -euo pipefail

# Print the path under "$base" whose immediate subdirectory name sorts last
# (by `sort -V`) and which contains the required marker file/dir. Empty if
# nothing matches. Uses bash arrays + nullglob so paths with spaces (like
# "C:\Program Files") work correctly — a plain glob in `ls $pattern` would
# word-split on the space.
#
# Args:  $1 = base directory
#        $2 = relative subpath that must exist inside each candidate
#             (e.g. "avr/bin/avr-gcc.exe" or "gnuBins/GnuWin32/bin/make.exe")
find_latest_with() {
    local base="$1"
    local marker="$2"
    [[ -d "$base" ]] || return 0

    local -a candidates=()
    shopt -s nullglob
    local entry
    for entry in "$base"/*/; do
        if [[ -e "${entry%/}/$marker" ]]; then
            candidates+=( "${entry%/}" )
        fi
    done
    shopt -u nullglob

    [[ ${#candidates[@]} -eq 0 ]] && return 0
    printf '%s\n' "${candidates[@]}" | sort -V | tail -n 1
}

UNAME="$(uname -s 2>/dev/null || echo unknown)"
case "$UNAME" in
    MINGW*|MSYS*|CYGWIN*)
        # Search both Program Files locations so 32-bit XC8 installs still work.
        xc8_root=""
        for root in "/c/Program Files/Microchip/xc8" "/c/Program Files (x86)/Microchip/xc8"; do
            candidate=$(find_latest_with "$root" "avr/bin/avr-gcc.exe")
            [[ -n "$candidate" ]] && xc8_root="$candidate"
        done
        if [[ -z "$xc8_root" ]]; then
            echo "build.sh: could not find avr-gcc under any" >&2
            echo "          C:\\Program Files[ (x86)]\\Microchip\\xc8\\<version>\\avr\\bin\\" >&2
            echo "          Install MPLAB XC8 for AVR and retry." >&2
            exit 1
        fi
        xc8_bin="$xc8_root/avr/bin"

        mplabx_root=""
        for root in "/c/Program Files/Microchip/MPLABX" "/c/Program Files (x86)/Microchip/MPLABX"; do
            candidate=$(find_latest_with "$root" "gnuBins/GnuWin32/bin/make.exe")
            [[ -n "$candidate" ]] && mplabx_root="$candidate"
        done
        if [[ -z "$mplabx_root" ]]; then
            echo "build.sh: could not find make.exe under any" >&2
            echo "          C:\\Program Files[ (x86)]\\Microchip\\MPLABX\\<version>\\gnuBins\\GnuWin32\\bin\\" >&2
            echo "          Install MPLAB X IDE and retry." >&2
            exit 1
        fi
        make_bin="$mplabx_root/gnuBins/GnuWin32/bin"

        dfp_dir=$(find_latest_with "$mplabx_root/packs/Microchip/AVR-Ex_DFP" "include")
        if [[ -z "$dfp_dir" ]]; then
            echo "build.sh: could not find AVR-Ex DFP under" >&2
            echo "          $mplabx_root/packs/Microchip/AVR-Ex_DFP/" >&2
            echo "          In MPLAB X, open Tools > Packs and install AVR-Ex_DFP." >&2
            exit 1
        fi

        export PATH="$xc8_bin:$make_bin:$PATH"

        # Honour an explicit DFP=... on the command line; otherwise inject the
        # one we discovered. We scan the existing args rather than appending
        # unconditionally so the user's override wins.
        has_dfp_override=0
        for arg in "$@"; do
            if [[ "$arg" == DFP=* ]]; then has_dfp_override=1; break; fi
        done
        if [[ $has_dfp_override -eq 0 ]]; then
            set -- "$@" "DFP=$dfp_dir"
        fi

        echo "build.sh: avr-gcc  = $xc8_bin"
        echo "build.sh: make.exe = $make_bin"
        if [[ $has_dfp_override -eq 0 ]]; then
            echo "build.sh: DFP      = $dfp_dir"
        else
            echo "build.sh: DFP      = (overridden on command line)"
        fi
        ;;
    *)
        # Linux / macOS / WSL: trust PATH, forward args unchanged.
        ;;
esac

exec make "$@"
