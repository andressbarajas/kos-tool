#!/bin/bash
#
# Build all kosload targets.
#
# Requirements:
#   - DC: SH-ELF cross-compiler (set DC_TOOLCHAIN env var or edit mk/toolchains.mk)
#   - GC: PPC cross-compiler (set GC_TOOLCHAIN env var or edit mk/toolchains.mk)
#   - libelf development headers (for host build)
#
# Usage:
#   ./build.sh                         # DC + GC clients + host
#   ./build.sh --host-only             # Host only
#   ./build.sh --dc-only               # DC clients only
#   ./build.sh --gc-only               # GC clients only
#   ./build.sh --dist                  # All clients + delivery artifacts (CDI/DOL/ISO/WAD)
#
set -e

HOST_ONLY=0
DC_ONLY=0
GC_ONLY=0
DIST=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --host-only)
            HOST_ONLY=1
            shift
            ;;
        --dc-only)
            DC_ONLY=1
            shift
            ;;
        --gc-only)
            GC_ONLY=1
            shift
            ;;
        --dist)
            DIST=1
            shift
            ;;
        *)
            echo "Unknown option: $1"
            echo "Usage: $0 [--host-only] [--dc-only] [--gc-only] [--dist]"
            exit 1
            ;;
    esac
done

if [ "$HOST_ONLY" -eq 1 ]; then
    echo "=== Building host tool ==="
    make host
    echo ""
    echo "Done: host/build/kos-tool"
    exit 0
fi

if [ "$DC_ONLY" -eq 1 ]; then
    echo "=== Building DC clients ==="
    make dc
    if [ "$DIST" -eq 1 ]; then
        echo ""
        echo "=== Building DC disc images ==="
        make dist-dc
    fi
    echo ""
    echo "Done:"
    echo "  DC serial: client/dreamcast/build/serial/dc-load-ser.elf"
    echo "  DC IP:     client/dreamcast/build/ip/dc-load-ip.elf"
    exit 0
fi

if [ "$GC_ONLY" -eq 1 ]; then
    echo "=== Building GC clients ==="
    make gc
    if [ "$DIST" -eq 1 ]; then
        echo ""
        echo "=== Building GC disc images ==="
        make dist-gc
    fi
    echo ""
    echo "Done:"
    echo "  GC serial: client/gamecube/build/serial/gc-load-ser.elf"
    echo "  GC IP:     client/gamecube/build/ip/gc-load-ip.elf"
    exit 0
fi

if [ "$DIST" -eq 1 ]; then
    echo "=== Building all targets + delivery artifacts ==="
    make all
    echo ""
    echo "=== Building delivery artifacts ==="
    make dist
    echo ""
    echo "Done:"
    echo "  Host:      host/build/kos-tool"
    echo "  DC serial: client/dreamcast/build/serial/dc-load-ser.elf"
    echo "  DC IP:     client/dreamcast/build/ip/dc-load-ip.elf"
    echo "  GC serial: client/gamecube/build/serial/gc-load-ser.elf"
    echo "  GC IP:     client/gamecube/build/ip/gc-load-ip.elf"
    echo "  Delivery artifacts: build/*.cdi build/*.dol build/*.iso build/*.wad"
    exit 0
fi

# Default: build everything
echo "=== Building all targets ==="
make all

echo ""
echo "Done:"
echo "  Host:      host/build/kos-tool"
echo "  DC serial: client/dreamcast/build/serial/dc-load-ser.elf"
echo "  DC IP:     client/dreamcast/build/ip/dc-load-ip.elf"
echo "  GC serial: client/gamecube/build/serial/gc-load-ser.elf"
echo "  GC IP:     client/gamecube/build/ip/gc-load-ip.elf"
