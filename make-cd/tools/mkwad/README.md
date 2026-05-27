# mkwad — Wii channel WAD packer

Packs a Wii boot DOL (typically `wii-load-ip.dol`) into a fakesigned
installable channel WAD.  Invoked from `make-cd`'s `wii` target, which is
in turn driven by the top-level `make disc-wii`.

## Required: the Wii common key

The Wii common key is a 16-byte constant Nintendo burned into every retail
Hollywood SoC's OTP fuses.  It's required to wrap the per-title key inside
the WAD ticket, so without it mkwad refuses to run.  You provide it
yourself — see the "Where to get the key" section below.

mkwad reads it from `make-cd/tools/mkwad/common-key.bin`.

`common-key.bin` is gitignored (covered by the repo-wide `*.bin`).

## Accepted file shapes

Auto-detected — drop whichever is convenient.  The bytes shown below are
**placeholder** (`00 11 … ff`), not a real key; substitute your own.

| Form | Example contents |
|---|---|
| Raw 16-byte binary | (literally 16 bytes) |
| Hex string | `00112233445566778899aabbccddeeff` |
| Spaced hex | `00 11 22 33 44 55 66 77 88 99 aa bb cc dd ee ff` |
| C-array | `0x00,0x11,0x22,0x33,...,0xff` |

Whitespace, commas, semicolons, and `0x`/`0X` byte prefixes are ignored.
Anything else (e.g. a `key:` prose prefix) is a hard error so a typo'd
file fails loudly instead of silently producing a wrong WAD.

## Where to get the key

The common key is **identical across every retail Wii** — it's a hardware
constant, not per-console — and it lives in Hollywood OTP, readable only
by Starlet (IOS), not by Broadway (the PPC).  So extracting it requires
code running on Starlet.  The canonical path:

1. Install [BootMii](https://wiibrew.org/wiki/BootMii) on your own Wii
   (typically via the HackMii Installer, launched from
   Letterbomb / Str2Hax / BlueBomb / FlashHax depending on your SM
   version).
2. From BootMii's menu, choose "Backup MINI" / "Backup keys".
3. Pull the SD card.  You now have `keys.bin` (OTP + SEEPROM dump).
4. Extract the 16 common-key bytes from the offset documented at
   [WiiBrew Hollywood/OTP](https://wiibrew.org/wiki/Hollywood/OTP) (the
   page lays out the full OTP word map and where the common key sits
   inside BootMii's `keys.bin`).

5. Save those 16 bytes into `make-cd/tools/mkwad/common-key.bin`:

   ```sh
   dd if=keys.bin of=make-cd/tools/mkwad/common-key.bin \
       bs=1 skip=<decimal-offset> count=16
   ```

   You can sanity-check by also extracting your per-console **NG-id**
   (also in the OTP map) — it's unique per Wii, so a nonzero value
   confirms the dump came from your hardware.

## Building & invoking

```sh
make disc-wii                       # full path: builds firmware + WAD
make -C make-cd wii                 # WAD only (firmware must exist)
```

Override defaults from the top-level Makefile:

```sh
make disc-wii WII_WAD_TITLE_ID=KOSL WII_WAD_IOS=58 \
    WII_WAD_NAME="wii-load-ip" WII_WAD_TITLE_VER=2
```

Bump `WII_WAD_TITLE_VER` to force the System Menu to overwrite an installed
copy of the same title-id.
