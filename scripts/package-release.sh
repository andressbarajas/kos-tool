#!/usr/bin/env bash
#
# package-release.sh — assemble kosload release archives from build/.
#
# Produces two kinds of artifact under build/release/:
#
#   Host archive (per OS/arch):
#     kos-tool-<version>-<platform>.tar.gz   (Linux/macOS)
#     kos-tool-<version>-<platform>.zip       (Windows)
#       -> the kos-tool binary + README + LICENSE
#
#   Firmware bundle (OS-independent):
#     kosload-<version>-firmware.zip
#       -> one directory per console holding its bootable images
#          (CDI/ISO/DOL/WAD/XISO), its raw loader binaries (ELF/BIN/DOL), and
#          its examples/ programs, plus README + LICENSE at the top level
#
# This script only PACKAGES what is already in build/; it does not build.
# The `make release*` targets declare the build dependencies.  CI's release
# job runs the host mode on each OS runner and the firmware mode once.
#
# Usage:
#   scripts/package-release.sh [--all | --host | --firmware]
#
# The version is taken from $KOSLOAD_VERSION if set (the Makefiles export it),
# otherwise parsed from mk/version.mk so the script also works standalone.

set -euo pipefail

mode="all"
case "${1:-}" in
    ""|--all)   mode="all" ;;
    --host)     mode="host" ;;
    --firmware) mode="firmware" ;;
    *) echo "usage: $0 [--all|--host|--firmware]" >&2; exit 2 ;;
esac

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILDDIR="$ROOT/build"
OUTDIR="$BUILDDIR/release"

# ---------- version ----------
# The Makefiles export KOSLOAD_VERSION (already expanded); standalone, ask make
# for it so version.mk stays the single source of truth (KOSLOAD_VERSION there
# is composed from MAJOR/MINOR/PATCH, so it can't be sed'd literally).
VERSION="${KOSLOAD_VERSION:-}"
if [ -z "$VERSION" ]; then
    VERSION="$(make -s -C "$ROOT" print-version 2>/dev/null || true)"
fi
if [ -z "$VERSION" ]; then
    echo "package-release: could not determine version" >&2
    exit 1
fi

# ---------- platform (host archive naming) ----------
EXE=""
case "$(uname -s)" in
    Darwin)              PLATFORM="macos-$(uname -m)" ;;
    Linux)               PLATFORM="linux-$(uname -m)" ;;
    MINGW*|MSYS*|CYGWIN*) PLATFORM="windows-$(uname -m)"; EXE=".exe" ;;
    *)                   PLATFORM="$(uname -s)-$(uname -m)" ;;
esac

mkdir -p "$OUTDIR"

# copy_into <dest-dir> <src>...   — copy each src that exists; ignore the rest.
# Unmatched globs arrive as literal paths and are skipped by the -e test.
copy_into() {
    local dest="$1"; shift
    local src
    for src in "$@"; do
        if [ -e "$src" ]; then
            cp -R "$src" "$dest/"
        fi
    done
    return 0
}

# Stamps the release version into each filename before its extension (dc-load-ip.bin ->
# dc-load-ip-3.0.1.bin).
copy_versioned() {
    local dest="$1"; shift
    local src base stem ext
    for src in "$@"; do
        [ -e "$src" ] || continue
        base="$(basename "$src")"
        ext="${base##*.}"
        if [ "$ext" = "$base" ]; then          # no extension
            cp -R "$src" "$dest/$base-$VERSION"
        else
            stem="${base%.*}"
            cp -R "$src" "$dest/$stem-$VERSION.$ext"
        fi
    done
    return 0
}

# archive_dir <stage-parent> <name> <ext>  — make <name>.<ext> from <stage>/<name>.
archive_dir() {
    local parent="$1" name="$2" ext="$3"
    ( cd "$parent"
      case "$ext" in
        tar.gz) tar -czf "$OUTDIR/$name.tar.gz" "$name" ;;
        zip)    rm -f "$OUTDIR/$name.zip"; zip -rq "$OUTDIR/$name.zip" "$name" ;;
        *) echo "package-release: unknown archive ext $ext" >&2; exit 1 ;;
      esac )
    echo "  PACKAGE build/release/$name.$ext"
}

# ============================================================
# Host archive
# ============================================================
package_host() {
    local bin="$BUILDDIR/kos-tool$EXE"
    if [ ! -x "$bin" ]; then
        echo "package-release: $bin not found (build the host tool first)" >&2
        exit 1
    fi

    local name="kos-tool-$VERSION-$PLATFORM"
    local stage="$OUTDIR/.stage-host/$name"
    rm -rf "$OUTDIR/.stage-host"
    mkdir -p "$stage"

    cp "$bin" "$stage/"
    copy_into "$stage" "$ROOT/README.md" "$ROOT/LICENSE"

    if [ -n "$EXE" ]; then
        archive_dir "$OUTDIR/.stage-host" "$name" zip
    else
        archive_dir "$OUTDIR/.stage-host" "$name" tar.gz
    fi
    rm -rf "$OUTDIR/.stage-host"
}

# ============================================================
# Firmware bundle
# ============================================================
package_firmware() {
    local name="kosload-$VERSION-firmware"
    local stage="$OUTDIR/.stage-fw/$name"
    rm -rf "$OUTDIR/.stage-fw"
    mkdir -p "$stage/dreamcast" "$stage/gamecube" "$stage/wii" \
             "$stage/playstation2" "$stage/xbox"

    # Bootable deliverables + raw loader binaries per console (skipped if
    # absent).  Sources are build/<short>/; the bundle keeps the long console
    # names.  Dreamcast ships .cdi plus raw .elf/.bin (its analog of the
    # GC/Wii .dol and PS2 .elf).
    copy_versioned "$stage/dreamcast"      "$BUILDDIR"/dc/dc-load-*.cdi \
                                           "$BUILDDIR"/dc/dc-load-*.elf "$BUILDDIR"/dc/dc-load-*.bin
    copy_versioned "$stage/gamecube"       "$BUILDDIR"/gc/gc-load-*.dol "$BUILDDIR"/gc/gc-load-*.iso
    copy_versioned "$stage/wii"            "$BUILDDIR"/wii/wii-load-*.dol "$BUILDDIR"/wii/wii-load-*.wad
    copy_versioned "$stage/playstation2"   "$BUILDDIR"/ps2/ps2-load-ip*.iso "$BUILDDIR"/ps2/ps2-load-ip.elf
    copy_into "$stage/xbox"                "$BUILDDIR"/xbox/default.xbe
    copy_versioned "$stage/xbox"           "$BUILDDIR"/xbox/xbox-load-ip.xiso \
                                           "$BUILDDIR"/xbox/xbox-load-ip.exe \
                                           "$BUILDDIR"/xbox/xbox-load-ip.bin

    # build/<short>/examples/ -> <stage>/<long>/examples/
    local pair short long
    for pair in dc:dreamcast gc:gamecube wii:wii ps2:playstation2 xbox:xbox; do
        short="${pair%%:*}"
        long="${pair##*:}"
        if [ -d "$BUILDDIR/$short/examples" ]; then
            copy_into "$stage/$long" "$BUILDDIR/$short/examples"
        fi
    done

    # Drop any console dir that ended up empty (toolchain wasn't installed).
    local d
    for d in dreamcast gamecube wii playstation2 xbox; do
        rmdir "$stage/$d" 2>/dev/null || true
    done

    copy_into "$stage" "$ROOT/README.md" "$ROOT/LICENSE"

    archive_dir "$OUTDIR/.stage-fw" "$name" zip
    rm -rf "$OUTDIR/.stage-fw"
}

case "$mode" in
    host)     package_host ;;
    firmware) package_firmware ;;
    all)      package_host; package_firmware ;;
esac
