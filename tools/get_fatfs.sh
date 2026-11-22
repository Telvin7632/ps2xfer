#!/usr/bin/env bash
# Downloads ChaN's FatFs R0.16 into iop/ and configures ffconf.h for ps2xfer:
# exFAT, LBA64, f_expand (contiguous preallocation), UTF-8 API, timestamps.
# Based on tools/get_fatfs.sh of ps2-exfat-format.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$ROOT/iop"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

ok=0
for ver in 16; do
    url="https://elm-chan.org/fsw/ff/arc/ff${ver}.zip"
    echo ">> trying $url"
    if curl -fsSL "$url" -o "$TMP/ff.zip"; then ok=1; break; fi
done
if [ "$ok" = 1 ]; then
    unzip -q "$TMP/ff.zip" -d "$TMP/ff"
else
    echo ">> elm-chan.org unavailable, using the GitHub mirror (abbrev/fatfs)"
    if ! git clone -q --depth 1 https://github.com/abbrev/fatfs "$TMP/ff"; then
        echo "!! Could not download FatFs. Get R0.16 from https://elm-chan.org/fsw/ff/"
        echo "   and copy ff.c ff.h ffconf.h diskio.h ffunicode.c from its source/ folder into iop/"
        exit 1
    fi
fi
SRC="$(dirname "$(find "$TMP/ff" -name ff.c | head -n 1)")"
cp "$SRC"/{ff.c,ff.h,ffconf.h,diskio.h,ffunicode.c} "$DEST/"

CONF="$DEST/ffconf.h"
set_opt() {
    if ! grep -Eq "^#define[[:space:]]+$1[[:space:]]" "$CONF"; then
        echo "!! option $1 not found in ffconf.h"; exit 1
    fi
    sed -i -E "s|^#define[[:space:]]+$1[[:space:]].*|#define $1\t$2|" "$CONF"
}

set_opt FF_FS_READONLY 0
set_opt FF_FS_MINIMIZE 0
set_opt FF_USE_MKFS    0            # host tests switch this on in their own copy
set_opt FF_USE_EXPAND  1            # f_expand: contiguous preallocation
set_opt FF_USE_FASTSEEK 0
set_opt FF_USE_LFN     1            # static LFN buffer: FatFs is used under px_fs_lock()
set_opt FF_LFN_UNICODE 2            # UTF-8 API (exFAT stores UTF-16 names)
set_opt FF_CODE_PAGE   437          # only for SFN; exFAT has none
set_opt FF_FS_EXFAT    1
set_opt FF_LBA64       1
set_opt FF_MIN_GPT     0x100000000  # same as the formatter
set_opt FF_FS_TINY     0
set_opt FF_FS_NORTC    0            # get_fattime() returns the PC's time from HELLO
set_opt FF_FS_LOCK     0
set_opt FF_FS_REENTRANT 0
set_opt FF_VOLUMES     1

echo ">> FatFs ready in iop/ ($(grep -m1 -o 'R0\.[0-9a-z]*' "$DEST/ff.h" || echo '?'))"
