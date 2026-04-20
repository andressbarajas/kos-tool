# kos-tool 3.0.0

**kos-tool** is a unified console loader for the **Sega Dreamcast** and **Nintendo GameCube**, combining the functionality of [dcload-serial](https://github.com/KallistiOS/dcload-serial) and [dcload-ip](https://github.com/KallistiOS/dcload-ip) into a single, clean codebase. It is a derivative of the original **dcload** by [Andrew Kieschnick](http://napalm-x.thegypsy.com/andrewk/dc/) (ADK/Napalm).

**kos-tool** is a set of programs used to send and receive data between your computer and your console. The classic use case is uploading and debugging homebrew programs. It consists of two parts:

* `kos-tool` — the host tool, run on your computer (Linux, macOS, Windows)
* `dc-load-serial` / `dc-load-ip` — the Dreamcast client firmware
* `gc-load-serial` / `gc-load-ip` — the GameCube client firmware

## Supported Hardware

### Dreamcast

| Connection | Client Firmware | Adapter |
|---|---|---|
| Serial | `dc-load-serial` | Coders Cable (RS-232 or USB-Serial) |
| Ethernet | `dc-load-ip` | Broadband Adapter (HIT-0400) or LAN Adapter (HIT-0300) |

### GameCube

| Connection | Client Firmware | Adapter |
|---|---|---|
| Serial | `gc-load-serial` | USB Gecko |
| Ethernet | `gc-load-ip` | Broadband Adapter (DOL-015) or ENC28J60 SPI adapter |

## Improvements over dcload-serial / dcload-ip

### Unified Codebase
* **Single host tool** — `kos-tool` replaces both `dc-tool-ser` and `dc-tool-ip` with one binary that handles serial and network transports for both Dreamcast and GameCube
* **Shared client code** — common client logic (main loop, screensaver, exception handler, CDFS, syscall table) is shared across all four firmware variants via a platform abstraction layer (`target_ops_t`)
* **Unified build system** — one `make` builds everything: DC firmware, GC firmware, host tool, examples, and disc images
* **Documented extension points** — see [`docs/architecture.md`](docs/architecture.md) for the console and transport boundaries used by new ports

### Firmware Update
* **Embedded firmware** — all four client firmware binaries (DC serial, DC IP, GC serial, GC IP) are embedded directly in the `kos-tool` host binary
* **Opt-in auto-update** — with `-F`, when `kos-tool` connects to a console running an older (or legacy dcload) firmware, it uploads and installs the new firmware via architecture-specific trampolines (SH4 and PPC)
* **IP config preservation** — during network firmware updates, DHCP/static IP settings are detected and patched into the new firmware so the console stays reachable
* **Legacy dcload compatibility** — auto-update works on consoles still running the original `dcload-serial` or `dcload-ip`, upgrading them in-place
* **Manual update** — `-U <file>` flag for updating from an external firmware binary

### GameCube Support
* **Full GameCube port** — serial transport via USB Gecko (EXI channel 1), network transport via Broadband Adapter (BBA) and ENC28J60 SPI adapter
* **Shared protocol** — GameCube uses the same serial and network protocols as Dreamcast, so the host tool works identically for both consoles
* **DHCP support** — GameCube network firmware supports DHCP, same as Dreamcast

### Host Tool Enhancements
* **Automatic transport detection** — if `-T` is not specified, `kos-tool` auto-detects whether the target is a serial device or network address
* **Network auto-discovery** — `-t auto` broadcasts on the LAN to find consoles (tries both legacy port 31313 and new port 53535)
* **addr2line integration** — `-A` flag decodes hex addresses in console output to `function:line` annotations using `addr2line`, with automatic toolchain prefix detection from the ELF's `e_machine` field (SH4 or PPC)
* **RTC sync** — `-w` flag sets the console's real-time clock to the host's local time
* **Program arguments** — `-- arg1 arg2` passes arguments to the loaded program
* **Larger network payloads** — 1440-byte payloads (up from 1024 in legacy dcload-ip), with automatic fallback to 1024-byte legacy mode for older firmware

### Client Firmware Enhancements
* **Screensaver** — bouncing bitmap icon activates after 30 seconds of idle, deactivates automatically on any command
* **Version and capability reporting** — firmware reports its version string and capability flags to the host on connect, enabling protocol negotiation
* **Performance counters** — Dreamcast firmware exposes SH4 performance counter access via syscalls

### Build System
* **Simple Makefiles** — no CMake dependency; just `make` with the cross-compiler toolchains in PATH
* **Disc image generation** — `make disc` produces bootable CDI (Dreamcast) and ISO (GameCube) disc images
* **Example programs** — 10 freestanding test programs (console, rainbow, syscall, CDFS, exception, maple, etc.) built automatically

## Features

* Load `elf`, `srec`, and `bin` files (serial transfers are LZO-compressed)
* PC I/O — read/write files on the host from the console (KOS `/pc/` filesystem)
* CDFS redirection — redirect GD-ROM reads to an ISO image on the host
* GDB remote debugging via GDB-over-dcload
* Exception handler with full register dump
* Auto-discovery of network devices (`-t auto`)
* Supports both BBA and LAN Adapter in a single binary (Dreamcast IP)
* Runtime baud rate negotiation up to 1.5Mbaud (Dreamcast serial)
* Cross-platform host tool: Linux, macOS, Windows (MSYS2)

## Building

### Requirements

* A C compiler for the host (GCC, Clang, or MinGW)
* `libelf` development headers
* Cross-compiler toolchains for client builds:
  * Dreamcast: `sh-elf-gcc`
  * GameCube: `powerpc-eabi-gcc`

### Build Everything

```bash
make        # builds DC firmware, GC firmware, host tool, and examples
```

Output goes to `build/`:
```
build/
├── kos-tool              # host tool (with embedded firmware)
├── dc-load-ser.{elf,bin} # Dreamcast serial firmware
├── dc-load-ip.{elf,bin}  # Dreamcast network firmware
├── gc-load-ser.{elf,bin} # GameCube serial firmware
├── gc-load-ip.{elf,bin}  # GameCube network firmware
└── examples/
    ├── dc/               # Dreamcast example ELFs
    └── gc/               # GameCube example ELFs
```

### Individual Targets

```bash
make dc      # Dreamcast firmware + examples + rebuild kos-tool
make gc      # GameCube firmware + examples + rebuild kos-tool
make host    # Host tool only (embeds whatever firmware bins exist)
make disc    # Bootable disc images (CDI for DC, ISO for GC)
```

### Platform-Specific Notes

```bash
# Linux
sudo apt-get install libelf-dev

# macOS
brew install libelf

# Windows (MSYS2 MINGW64)
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-libelf make
```

### VGA-Only (Naomi / System SP)

Arcade boards like Naomi and System SP don't have standard A/V cable detect wiring. To build Dreamcast firmware that forces VGA output and skips cable detection:

```bash
make dc VGAONLY=1
```

### Static IP (Dreamcast/GameCube)

By default, network firmware uses DHCP. To build with a static IP:

```bash
make dc STATICIP=192.168.1.100
make gc STATICIP=192.168.1.100
```

## Usage

```
kos-tool [options]

Transports:
  -T serial    Use serial transport
  -T network   Use network transport

Commands:
  -x <file>    Upload and execute <file>
     [-- args] Pass arguments to loaded program (must be last)
  -u <file>    Upload <file>
  -d <file>    Download to <file>
  -r           Reset console

Options:
  -a <addr>    Set address (default: 0x8c010000)
  -s <size>    Set size for download
  -t <device>  Serial device, IP address, or 'auto'
  -b <baud>    Serial baud rate (default: 57600)
  -n           Disable console/fileserver
  -p           Dumb terminal mode
  -q           Quiet mode
  -c <path>    Chroot to <path>
  -m <path>    Map /pc/ to <path>
  -i <iso>     Enable CDFS redirection with <iso>
  -g           Start GDB server on port 2159
  -e           Alternate 115200/230400 baud
  -E           Use external clock
  -l           Force legacy 1024-byte payloads
  -f           Fast mode (no FIFO delays)
  -w           Sync console RTC to host time
  -U <file>    Update firmware from external file
  -F           Enable automatic firmware update
  -A [prefix]  Decode addresses via addr2line
```

### Examples

```bash
# Dreamcast serial — upload and run a program
kos-tool -t /dev/ttyUSB0 -x program.elf

# Dreamcast serial — high baud rate on macOS
kos-tool -t /dev/cu.usbserial-A50285BI -b 1500000 -x program.elf

# Dreamcast serial — Windows
kos-tool -t COM4 -b 500000 -x program.elf

# Dreamcast network — upload and run
kos-tool -t 192.168.1.100 -x program.elf

# Dreamcast network — auto-discover device on LAN
kos-tool -t auto -x program.elf

# Dreamcast network — with CDFS redirection
kos-tool -t 192.168.1.100 -i game.iso -x program.elf

# GameCube serial — upload and run
kos-tool -t /dev/ttyUSB0 -x program.elf

# GameCube network — upload and run
kos-tool -t 192.168.1.50 -x program.elf

# addr2line — auto-detect toolchain from ELF
kos-tool -t /dev/ttyUSB0 -A -x program.elf

# GDB debugging session
kos-tool -t /dev/ttyUSB0 -g -x program.elf
# Then in another terminal: sh-elf-gdb -ex "target remote :2159" program.elf
```

## On-Screen Display

When the client firmware boots, it displays:

```
  dc-load-ip 3.0.0
  Broadband Adapter (HIT-0400)
  00:d0:de:ad:be:ef
  192.168.1.100
  idle...
```

* Blue background = Broadband Adapter, Green = LAN Adapter, Red = no adapter detected
* After 30 seconds of inactivity, a screensaver activates (bouncing icon)
* The screensaver deactivates automatically when a command is received

## Serial Speed Notes

To attain the highest speeds with serial, you need a USB-Serial adapter that matches the SH4's generated baud rates. Above 115200, the SH4 generates: `223214`, `260416`, `312500`, `390625`, `520833`, `781250`, `1562500`.

* **Silicon Labs CP2102N** — best match with SH4, reliably hits 1.56Mbaud
* **FTDI FT232R** — works up to 781250; 1.56Mbaud depends on chip tolerance

## KOS GDB-over-dcload

To debug Dreamcast programs remotely:

1. Build your KOS program with `-g` and add a `gdb_init()` call in `main()`
2. Launch with `kos-tool -t <device> -g -x <program.elf>`
3. In another terminal: `sh-elf-gdb -ex "target remote :2159" <program.elf>`

## Project Structure

```
kos-tool/
├── client/              # Console-side firmware
│   ├── common/          # Shared client code (commands, main loop)
│   ├── serial/          # Serial transport
│   ├── network/         # Network transport
│   ├── include/         # Client headers
│   ├── dreamcast/       # DC platform (SCIF, video, BBA/LA drivers)
│   ├── gamecube/        # GC platform (USBGecko, EXI, BBA/ENC28J60 drivers)
│   └── examples/        # Freestanding test programs
├── host/                # Host tool (kos-tool)
│   ├── src/             # Main, console, upload/download, discovery
│   └── include/         # Host headers
├── include/             # Shared protocol headers
├── make-cd/             # Disc image tools (CDI, ISO/DOL)
├── third_party/         # minilzo, mingw-libelf
└── mk/                  # Build configuration
```

## Credits

kos-tool is derived from the original dcload by Andrew Kieschnick (ADK/Napalm), with major contributions from the KallistiOS community over two decades.

* **Andrew Kieschnick** (ADK/Napalm) — original dcload author
* **Marcus Comstedt** — `video.s`, `maple.c`/`maple.h`
* **Megan Potter** — RTL8139 driver base
* **Moopthehedgehog** — LAN Adapter overhaul, DHCP, perf counters, performance work (dcload-ip)
* **Mickaël Cardoso** (SiZiOUS) — extensive fixes and maintenance
* **Florian Schulze** (Proff) — Win32 porting
* **Paul Boese** (Axlen) — serial protocol endian fixes
* **Dan Potter** — macOS fixes
* **Eric Fradella** (darcagn) — DHCP retry
* **Lawrence Sebald**, **Donald Haase**, **Falco Girgis** — KallistiOS team contributions
* **Andy Barajas** — kos-tool unification, GameCube support

See the legacy [dcload-serial](https://github.com/KallistiOS/dcload-serial) and [dcload-ip](https://github.com/KallistiOS/dcload-ip) repositories for the full history of contributors.

## License

This program is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License version 2 as published by the Free Software Foundation. See [COPYING](COPYING) for details.
