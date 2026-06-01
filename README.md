# kos-tool 3.0.0

**kos-tool** is a unified console loader for the **Sega Dreamcast**, **Nintendo GameCube**, **Sony PlayStation 2**, and **Nintendo Wii**, combining the functionality of [dcload-serial](https://github.com/KallistiOS/dcload-serial) and [dcload-ip](https://github.com/KallistiOS/dcload-ip) into a single, clean codebase. It is a derivative of the original **dcload** by [Andrew Kieschnick](http://napalm-x.thegypsy.com/andrewk/dc/) (ADK/Napalm).

**kos-tool** is a set of programs used to send and receive data between your computer and your console. The classic use case is uploading and debugging homebrew programs. It consists of two parts:

* `kos-tool` — the host tool, run on your computer (Linux, macOS, Windows)
* `dc-load-serial` / `dc-load-ip` — the Dreamcast client firmware
* `gc-load-serial` / `gc-load-ip` — the GameCube client firmware
* `ps2-load-ip` — the PlayStation 2 client firmware
* `wii-load-ip` — the Wii client firmware

## Supported Hardware

### Dreamcast

| Connection | Client Firmware | Adapter |
|---|---|---|
| Serial | `dc-load-serial` | Coders Cable (RS-232 or USB-Serial) |
| Ethernet | `dc-load-ip` | Broadband Adapter (HIT-0400), LAN Adapter (HIT-0300), or W5500 (SCIF-SPI) |

### GameCube

| Connection | Client Firmware | Adapter |
|---|---|---|
| Serial | `gc-load-serial` | USB Gecko |
| Ethernet | `gc-load-ip` | Broadband Adapter (DOL-015), ENC28J60 SPI adapter, or W5500 (EXI-SPI) |

### PlayStation 2

| Connection | Client Firmware | Adapter |
|---|---|---|
| Ethernet | `ps2-load-ip` | Network Adapter / Broadband Adapter (DEV9 SMAP) |

### Wii

| Connection | Client Firmware | Adapter |
|---|---|---|
| Ethernet | `wii-load-ip` | Wii LAN Adapter (RVL-015, USB) |
| Wi-Fi (experimental) | `wii-load-ip` | Internal Wi-Fi |

Both Wii paths run over the console's IOS network stack. The wired LAN
Adapter is the more reliable path; **internal Wi-Fi is experimental** — it
works, but is more sensitive to packet loss (the loader and host carry
syscall retransmit/dedup logic specifically to tolerate it). `wii-load-ip`
is launched from the [Homebrew Channel](https://wiibrew.org/wiki/Homebrew_Channel)
via the generated `wii-load-ip.dol`.

## Improvements over dcload-serial / dcload-ip

### Unified Codebase
* **Single host tool** — `kos-tool` replaces both `dc-tool-ser` and `dc-tool-ip` with one binary that handles serial and network transports across supported consoles
* **Shared client code** — common client logic (main loop, screensaver, exception handler, CDFS, syscall table) is shared across client firmware variants via a platform abstraction layer (`target_ops_t`)
* **Unified build system** — one `make` builds everything: DC firmware, GC firmware, PS2 firmware, host tool, examples, and delivery artifacts (CDI/ISO/WAD)
* **Documented extension points** — see [`docs/architecture.md`](docs/architecture.md) for the console and transport boundaries used by new ports

### Firmware Update
* **Embedded firmware** — all client firmware binaries (DC serial, DC IP, GC serial, GC IP, PS2 IP, Wii IP) are embedded directly in the `kos-tool` host binary
* **Opt-in auto-update** — with `-F`, when `kos-tool` connects to a console running an older (or legacy dcload) firmware, it uploads and installs the new firmware via architecture-specific trampolines (SH4, PPC, and MIPS R5900)
* **IP config preservation** — during network firmware updates, DHCP/static IP settings are detected and patched into the new firmware so the console stays reachable
* **Legacy dcload compatibility** — auto-update works on consoles still running the original `dcload-serial` or `dcload-ip`, upgrading them in-place
* **Manual update** — `-U <file>` flag for updating from an external firmware binary

### GameCube Support
* **Full GameCube port** — serial transport via USB Gecko (EXI channel 1), network transport via Broadband Adapter (BBA), ENC28J60 SPI adapter, and W5500 (EXI-SPI)
* **Shared protocol** — GameCube uses the same serial and network protocols as Dreamcast, so the host tool works identically for both consoles
* **DHCP support** — GameCube network firmware supports DHCP, same as Dreamcast

### PlayStation 2 Support
* **PS2 network loader** — Ethernet transport through the official DEV9 SMAP network hardware
* **Shared protocol** — PS2 uses the same network upload/download command protocol as Dreamcast and GameCube
* **DHCP and static IP support** — PS2 network firmware can discover an address with DHCP or be built with a fixed IP

### Wii Support
* **Wii network loader** — Ethernet transport over the Wii LAN Adapter (RVL-015) and, experimentally, the internal Wi-Fi, both via the console's IOS network stack
* **Homebrew Channel launch** — `wii-load-ip` ships as a `.dol` that boots from the Homebrew Channel
* **Installable System Menu channel** — `make dist-wii` packs the loader into a Wii channel WAD that installs to the System Menu and launches like a retail channel (no Homebrew Channel required)
* **Shared protocol** — Wii uses the same network upload/download command protocol as the other consoles
* **Lossy-link resilience** — the syscall path adds opt-in request sequencing with host-side reply dedup and bounded retransmit, so a dropped request or reply recovers instead of wedging — important on the experimental Wi-Fi path
* **DHCP support** — IOS handles address acquisition; the loader displays the current lease

### Host Tool Enhancements
* **Automatic transport detection** — `kos-tool` infers serial vs network from `-t <device|ip|dhcp>`
* **Network DHCP discovery** — `-t dhcp` broadcasts on the LAN to find consoles (tries both legacy port 31313 and new port 53535)
* **Automatic addr2line integration** — when the uploaded file is an ELF, console stack addresses are annotated automatically for SH4 (DC), PPC (GC/Wii), or MIPS (PS2). The `addr2line` path is derived from the toolchain location (`$DC_TOOLCHAIN`/`$GC_TOOLCHAIN`/`$PS2_EE_TOOLCHAIN`, else the build-time default)
* **RTC sync** — `-w` flag sets the console's real-time clock to the host's local time
* **Program arguments** — `-- arg1 arg2` passes arguments to the loaded program
* **Performance diagnostics** — `-P` / `--diag` prints transfer timing and recovery statistics
* **Larger network payloads** — 1440-byte payloads (up from 1024 in legacy dcload-ip), with automatic fallback to 1024-byte legacy mode for older firmware

### Client Firmware Enhancements
* **Screensaver** — bouncing bitmap icon activates after 30 seconds of idle, deactivates automatically on any command
* **Version and capability reporting** — firmware reports its version string and explicit capability flags to the host on connect, enabling feature negotiation
* **Performance counters** — Dreamcast firmware exposes SH4 performance counter access via syscalls

### Build System
* **Simple Makefiles** — no CMake dependency; just `make` with the cross-compiler toolchains in PATH
* **Delivery artifact generation** — `make dist` produces bootable CDI (Dreamcast) and ISO (GameCube) disc images, plus an installable Wii channel WAD, using in-tree image builders
* **Example programs** — freestanding test programs (console, rainbow, syscall, CDFS, exception, maple, etc.) built automatically

## Features

* Load `elf`, `srec`, and `bin` files (serial transfers are LZO-compressed)
* PC I/O — read/write files on the host from the console (KOS `/pc/` filesystem)
* Dreamcast CDFS redirection — redirect GD-ROM reads to an ISO image on the host
* GDB remote debugging via GDB-over-dcload
* Exception handler with full register dump
* DHCP/network discovery (`-t dhcp`)
* Network firmware supports BBA/LAN/W5500 on Dreamcast, BBA/ENC28J60/W5500 on GameCube, DEV9 SMAP on PlayStation 2, and LAN Adapter / Wi-Fi (experimental) on Wii
* Runtime baud rate negotiation up to 1.5Mbaud (Dreamcast serial)
* Cross-platform host tool: Linux, macOS, Windows (MSYS2)

## Building

### Requirements

* A C compiler for the host (GCC, Clang, or MinGW)
* `libelf` development headers
* Cross-compiler toolchains for client builds:
  * Dreamcast: `sh-elf-gcc`
  * GameCube: `powerpc-eabi-gcc`
  * Wii: `powerpc-eabi-gcc` (same toolchain as GameCube)
  * PlayStation 2 EE: `mips64r5900el-ps2-elf-gcc`
  * PlayStation 2 IOP: `mipsel-elf-gcc`

Toolchain locations are configured in [`mk/toolchains.mk`](mk/toolchains.mk).
If your Dreamcast, GameCube, Wii, or PlayStation 2 toolchains are not installed
under the default paths, edit that file before running `make dc`, `make gc`,
`make wii`, or `make ps2`.

### Build Everything

```bash
make        # builds DC firmware, GC firmware, PS2 firmware, Wii firmware, host tool, and examples
```

Output goes to `build/`:
```
build/
├── kos-tool                  # host tool (with embedded firmware)
├── dc-load-ser.{elf,bin}     # Dreamcast serial firmware
├── dc-load-ip.{elf,bin}      # Dreamcast network firmware
├── gc-load-ser.{elf,bin}     # GameCube serial firmware
├── gc-load-ip.{elf,bin}      # GameCube network firmware
├── ps2-load-ip.elf           # PlayStation 2 network firmware
├── wii-load-ip.{elf,bin,dol} # Wii network firmware (.dol boots from Homebrew Channel)
├── wii-load-ip.wad           # Wii installable channel (built by `make dist-wii`/`make dist`)
└── examples/
    ├── dc/                   # Dreamcast example ELFs
    ├── gc/                   # GameCube example ELFs
    ├── ps2/                  # PlayStation 2 example ELFs
    └── wii/                  # Wii example ELFs
```

### Individual Targets

```bash
make dc       # Dreamcast firmware + examples + rebuild kos-tool
make gc       # GameCube firmware + examples + rebuild kos-tool
make ps2      # PlayStation 2 firmware + examples + rebuild kos-tool
make wii      # Wii firmware + examples + rebuild kos-tool
make host     # Host tool only (embeds whatever firmware bins exist)
make dist     # Delivery artifacts: CDI (DC), ISO (GC), and Wii channel WAD
make dist-dc  # Dreamcast-only bootable CDI images
make dist-gc  # GameCube-only bootable ISO images
make dist-wii # Wii-only installable channel WAD
make gc-dol   # GameCube DOL files only (no ISO)
```

You can also override toolchains per invocation:

```bash
make gc GC_TOOLCHAIN=/path/to/powerpc-eabi/bin
make dc DC_TOOLCHAIN=/path/to/sh-elf/bin
make wii GC_TOOLCHAIN=/path/to/powerpc-eabi/bin
make ps2 PS2_EE_TOOLCHAIN=/path/to/mips64r5900el-ps2-elf/bin PS2_IOP_TOOLCHAIN=/path/to/mipsel-elf/bin
```

### Platform-Specific Notes

```bash
# Debian / Ubuntu
sudo apt-get install build-essential pkg-config libelf-dev

# macOS
brew install libelf

# Windows (MSYS2 MINGW64)
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-libelf make vim
```

### Windows

Build from the `MSYS2 MinGW x64` shell, not the plain `MSYS` shell.
The host build expects the MinGW toolchain (`/mingw64/bin/gcc`) to be on `PATH`,
which is set up automatically in `MINGW64`.

Embedding client firmware into `kos-tool` also requires `xxd`, which is provided
by the MSYS2 `vim` package.

USB serial adapters used with Dreamcast\GameCube coders cables should appear in Device
Manager as `COMx` ports. Pass that `COM` name directly to `kos-tool` with
`-t`, for example `-t COM3`.

Common Windows USB-serial drivers:

* Silicon Labs CP210x adapters: install the
  [CP210x Universal Windows Driver](https://www.silabs.com/software-and-tools/usb-to-uart-bridge-vcp-drivers?tab=downloads).
* FTDI adapters: install the
  [FTDI VCP driver](https://ftdichip.com/drivers/vcp-drivers/). In Device
  Manager, open the adapter properties and make sure **Load VCP** is enabled so
  Windows exposes the adapter as a `COMx` serial port.

### VGA-Only (Naomi / System SP)

Arcade boards like Naomi and System SP don't have standard A/V cable detect wiring. To build Dreamcast firmware that forces VGA output and skips cable detection:

```bash
make dc VGAONLY=1
```

### Static IP

By default, network firmware uses DHCP. To build with a static IP:

```bash
make dc STATICIP=192.168.1.100
make gc STATICIP=192.168.1.100
make ps2 STATICIP=192.168.1.100
```

The Wii has no `STATICIP` knob — IOS owns address acquisition, so a
patched-in static IP would be ignored. For a stable Wii address, set a
static IP in the Wii Settings UI (IOS picks it up via its NCD config) or
use a DHCP reservation on your router.

## Usage

```
kos-tool [options] -t <device|ip|dhcp>
kos-tool [options] -T <profile>

Commands:
  -x <file>    Upload and execute <file>
     [-- args] Pass arguments to loaded program (must be last)
  -u <file>    Upload <file>
  -d <file>    Download to <file>
  -r           Reset console

Options:
  -a <addr>    Set address (default: 0x8c010000)
  -s <size>    Set size for download
  -t <device>  Serial device, IP address, or 'dhcp'
  -T <profile> Use configured target profile (dc_serial, gc_serial, dc_ip, gc_ip, ps2_ip, wii_ip)
  -b <baud>    Serial baud rate (default: 57600)
  -n           Disable console/fileserver
  -p           Dumb terminal mode
  -q           Quiet mode
  -c <path>    Disabled (use -m <path> instead)
  -m <path>    Map /pc/ to <path>
  -i <iso>     Enable CDFS redirection with <iso>
  -g           Start GDB server on port 2159
  -e           Alternate 115200/230400 baud
  -E           Use external clock
  -l           Force legacy 1024-byte payloads
  -f           Fast mode (no FIFO delays)
  -P, --diag   Print performance diagnostics
  -w           Sync console RTC to host time
  -U <file>    Update firmware from external file
  -F           Enable automatic firmware update
  -h           Show this help
```

### Configured Targets

`kos-tool.cfg` is created next to the `kos-tool` binary. You can add target
profiles there and select them with `-T`:

```cfg
# Target profiles
dc_serial = /dev/ttyUSB0
gc_serial = /dev/ttyUSB1
dc_ip = 172.16.0.10
gc_ip = dhcp
ps2_ip = dhcp
wii_ip = dhcp
serial_baud = 1562500
```

Then run:

```bash
kos-tool -T dc_serial -x program.elf
kos-tool -T gc_ip -x program.elf
kos-tool -T ps2_ip -x program.elf
kos-tool -T wii_ip -x program.elf
```

`-t` overrides `-T` when both are present. `serial_baud` is used only for
serial targets, and `-b` overrides it.

### Examples

```bash
# Dreamcast serial — upload and run a program
kos-tool -t /dev/ttyUSB0 -x program.elf

# Dreamcast serial — high baud rate on macOS
kos-tool -t /dev/cu.usbserial-A50285BI -b 1500000 -x program.elf

# Dreamcast serial — Windows
kos-tool.exe -t COM4 -b 500000 -x program.elf

# Dreamcast network — upload and run
kos-tool -t 192.168.1.100 -x program.elf

# Dreamcast network — discover DHCP device on LAN
kos-tool -t dhcp -x program.elf

# Dreamcast network — with CDFS redirection
kos-tool -t 192.168.1.100 -i game.iso -x program.elf

# GameCube serial — upload and run
kos-tool -t /dev/ttyUSB0 -x program.elf

# GameCube network — upload and run
kos-tool -t 192.168.1.50 -x program.elf

# PlayStation 2 network — upload and run
kos-tool -t 172.16.0.10 -x program.elf

# PlayStation 2 network — discover DHCP device on LAN
kos-tool -t dhcp -x program.elf

# Wii network — upload and run (LAN Adapter or internal Wi-Fi)
kos-tool -t 192.168.1.95 -x program.elf

# Wii network — discover DHCP device on LAN
kos-tool -t dhcp -x program.elf

# Enable performance diagnostics
kos-tool -t dhcp --diag -x program.elf

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

* Blue background = Broadband Adapter, Green = LAN Adapter, dark cyan = W5500, Red = no adapter detected
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
│   ├── common/          # Shared client code
│   │   ├── core/        # Main loop, commands, screensaver, CDFS
│   │   ├── serial/      # Serial transport
│   │   ├── network/     # Network transport
│   │   └── drivers/     # Shared device drivers
│   ├── include/         # Client headers
│   ├── dreamcast/       # DC platform (SCIF, video, BBA/LA drivers)
│   ├── gamecube/        # GC platform (USBGecko, EXI, BBA/ENC28J60/W5500 drivers)
│   ├── playstation2/    # PS2 platform (DEV9, SMAP, IOP modules, video)
│   ├── wii/             # Wii platform (IOS net sockets, HBC stub, video)
│   └── examples/        # Freestanding test programs
├── host/                # Host tool (kos-tool)
│   ├── src/             # Main, console, upload/download, discovery
│   └── include/         # Host headers
├── include/             # Shared protocol headers
├── make-dist/           # Delivery artifact tools (CDI, ISO/DOL, WAD)
├── third_party/         # minilzo, mingw-libelf
└── mk/                  # Build configuration
```

## Credits

kos-tool is derived from the original dcload by Andrew Kieschnick (ADK/Napalm), with major contributions from the KallistiOS community over two decades.

* **Andrew Kieschnick** (ADK/Napalm) — original dcload author
* **Marcus Comstedt** — `video.s`, `maple.c`/`maple.h`
* **Megan Potter** — RTL8139 driver base, macOS fixes
* **Moopthehedgehog** — LAN Adapter overhaul, DHCP, perf counters, performance work (dcload-ip)
* **Mickaël Cardoso** (SiZiOUS) — extensive fixes and maintenance
* **Florian Schulze** (Proff) — Win32 porting
* **Paul Boese** (Axlen) — serial protocol endian fixes
* **Eric Fradella** (darcagn) — DHCP retry
* **Lawrence Sebald**, **Donald Haase**, **Falco Girgis** — KallistiOS team contributions
* **Andy Barajas** (BBHoodsta) — kos-tool unification, GameCube, PlayStation 2, and Wii support
* **Paul Cercueil** (pcercuei) - Testing, feedback, bug fixes
* **Ruslan Rostovtsev** (DC-SWAT) - W5500 driver
* **Extrems**, **emukidid** - Swiss
* **fail0verflow** - Homebrew Channel
* **fjtrujy** - PS2Link

See the legacy [dcload-serial](https://github.com/KallistiOS/dcload-serial) and [dcload-ip](https://github.com/KallistiOS/dcload-ip) repositories for the full history of contributors.

## License

This program is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License version 2 as published by the Free Software Foundation. See [COPYING](COPYING) for details.
