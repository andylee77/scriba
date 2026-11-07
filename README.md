# Scriba

A command-line flash programming tool for IP camera chips. Scriba reads, writes,
erases, and verifies SPI NOR, SPI NAND, and EEPROM memory using inexpensive USB
programmers. Built for the [Thingino](https://github.com/themactep/thingino-firmware) firmware project.

## Quick Start

```bash
# Build
git clone https://github.com/themactep/scriba.git && cd scriba
make && sudo make install

# Identify the flash chip
scriba -i

# Save a backup
scriba -R backup.bin

# Write new firmware
scriba -W firmware.bin
```

## Supported Programmers

Scriba auto-detects the connected programmer — no configuration needed.

| Programmer | VID:PID | |
|---|---|---|
| **CH341A** (black PCB) | `1a86:5512` | Must be modified for 3.3 V; see [CH341A guide](https://github.com/themactep/thingino-firmware/wiki/CH341A-Programmer) |
| **EZP2019** / **EZP2019+** | `1fc8:310b` / `310c` | Works out of the box |
| **EZP2023** | `1fc8:310d` | |
| **CH347T** / **CH347F** | `1a86:55db` / `55de` | Put a CH347T board in mode 1 (UART + SPI + I2C); the other modes have no usable SPI interface. SPI flash and SPI EEPROM only, no I2C or Microwire EEPROM yet. Hardware SPI clock, 15 MHz by default; change it with `--spi-speed` |

Force a specific programmer with `-P`:

```bash
scriba -P ch341a -i
scriba -P ch347 -i
scriba -P ezp2019 -i
```

Set the CH347 SPI clock with `--spi-speed <kHz>`. It picks the fastest of
60, 30, 15, 7.5, 3.75, 1.875, 0.9375 or 0.46875 MHz that does not exceed the
request; use a lower clock for long clip leads or in-circuit reads:

```bash
scriba -P ch347 --spi-speed 30000 -R backup.bin
```

In the web version, pick the clock from the CH347 selector next to the Erase
button. A change takes effect immediately; click Detect afterwards.

On CH347T firmware 0341 or newer (and any CH347F), scriba carries
chip-select inside the SPI data frames, as WCH's own Linux driver does,
which saves USB round trips on every flash command. The connect log shows the
firmware version and whether this is on. `--no-cs-flags` (web: untick
**Fast CS**) uses separate chip-select commands instead.

## Build & Install

**Prerequisites:** GCC, libusb-1.0 development files, make

```bash
make              # dynamic linking (needs libusb-1.0 installed)
make static       # static build (bundles libusb)
sudo make install # installs binary + udev rules, reloads udev
```

`make install` copies udev rules for all supported programmers and reloads
them. If your programmer was already plugged in, unplug and replug it after
installation.

## Web Version (WebUSB / WASM)

Scriba also runs in the browser via WebAssembly and WebUSB — no driver
installation needed. Requires a Chromium-based browser (Chrome, Edge, Opera).

On Windows, WebUSB can only open a USB interface bound to the WinUSB driver.
The CH347 is a composite device: bind WinUSB to its SPI interface only
(Interface 2 on a CH347T, Interface 4 on a CH347F), for example with
[Zadig](https://zadig.akeo.ie/) (Options → List All Devices). Leave the UART
interfaces on their serial driver.

The log shows where an image or a flash dump holds data: the address ranges
of 256-byte pages that are not all 0xFF, and where the blank (0xFF) tail
starts. Writes to NOR flash skip blank pages, since programming 0xFF changes
nothing. **Trim 0xFF tail** saves a read only up to its last data byte, as
`flash_dump_trimmed.bin`; writing that back loses nothing, because Write
erases the whole chip first. Keep full dumps as backups.

### Build

```bash
cd web
npm install
cd build && emcmake cmake .. && emmake make
```

The build outputs `scriba.js` and `scriba.wasm` to `web/public/wasm/`.

**Prerequisites:** Emscripten (`emcc`), CMake, Node.js

### Development

```bash
cd web
npm install
npm run dev      # starts Vite dev server on http://localhost:5173
```

Vite serves static files from `web/` including `web/public/wasm/` where the
Emscripten build places its output. After rebuilding (`emmake make`), just
reload the browser.

### Production

```bash
cd web
npm run build    # outputs to web/dist/
```

Serve the `web/dist/` directory with any static file server.

## Usage

```
scriba [options]

Automation:
  -R <file>    Read chip twice and compare — reliable backup
  -W <file>    Erase + write + verify — safe flash

Operations:
  -i           Read chip ID
  -e           Erase chip
  -r <file>    Read chip to file
  -w <file>    Write file to chip
  -v           Verify after write (use with -w)

Options:
  -a <addr>    Start address (hex or decimal)
  -l <bytes>   Length in bytes
  -L           List all supported chips

SPI NAND:
  -d           Disable on-die ECC
  -o <bytes>   Set OOB size (64–256)
  -I           Ignore ECC errors during read
  -k           Skip bad pages

EEPROM:
  -E <chip>    EEPROM type, e.g. 24c32, 93c46, 25q64
  -8           8-bit organization (Microwire)
  -f <bits>    Address size (Microwire)
  -s <bytes>   Page size (SPI EEPROM)

General:
  -P <prog>    Programmer: ch341a, ch347, ezp2019, auto (default)
  --spi-speed <kHz>  SPI clock limit (CH347 only; default 15000)
  --no-cs-flags  CH347: separate chip-select commands even on firmware 0341+
  --layout <file>  Show the partitions of an image file (no programmer)
  --debug      USB debug output
  --trace      Dump all SPI traffic
  -h           Help
```

## Partition layout

SPI NOR flash in cameras carries no partition table, so scriba works the
layout out from the image: names and boundaries from an `mtdparts=` string
(preferring one in a U-Boot environment whose CRC checks out), and each
partition's contents from its headers: Ingenic and SigmaStar boot images,
U-Boot environment, uImage, FIT/device tree, squashfs, JFFS2, UBI. With no
`mtdparts=` string the headers alone give the boundaries. The layout is
printed after a whole-chip read, by `--layout <file>` for any image file,
and in the web log after a read or when an image is loaded:

```text
Layout from mtdparts (jz_sfc) in the U-Boot env at 0x050000:
  0x000000-0x04FFFF  320 KB boot   Ingenic boot image (SPL + U-Boot), 186 KB used
  0x050000-0x05FFFF   64 KB env    U-Boot env (32 KB, CRC OK, 26 variables)
  0x060000-0x06FFFF   64 KB backup blank
  0x070000-0x1FFFFF 1600 KB kernel uImage "Linux-3.10.14__isvp_swan_1.0__" (kernel, lzma), 1.37 MB
  0x200000-0x7BFFFF 5888 KB rootfs squashfs 4.0 (xz), 5.72 MB used
  0x7C0000-0xFFFFFF 8448 KB data   JFFS2, 1 of 132 erase blocks in use
```

It also notes images that are not raw flash images (vendor upgrade
packages), layouts larger than the chip, and dumps that end inside the
layout.

## Examples

```bash
# Identify the chip
scriba -i

# Reliable backup — reads twice, compares, saves if identical
scriba -R backup.bin

# Safe flash — erases, writes, then verifies
scriba -W firmware.bin

# Single operations
scriba -r dump.bin -a 0 -l 0x400000    # read 4 MB from offset 0
scriba -w bootloader.bin -v            # write and verify
scriba -e                              # full chip erase

# Partition layout of a dump or firmware image
scriba --layout dump.bin

# Debugging
scriba --debug -i                      # see USB communication
scriba --trace -r dump.bin             # dump every SPI byte

# EEPROM
scriba -E 93c46 -r eeprom.bin
scriba -E 24c32 -w eeprom.bin
```

## CH347 test results

Tested with the web version (Chrome, Windows 11) on a CH347T programmer
board ("CH347 V1.0", firmware 0544, SPI interface bound to WinUSB with
Zadig) and a ZB25VQ128 (16 MB SPI NOR, ID `5E 40 18`) on a SOIC clip.

| Operation, 16 MB | Time |
|---|---|
| Read at 15 MHz | 11.3–11.9 s |
| Read at 60 MHz | 7.2 s |
| Chip erase | 29–32 s (set by the chip) |
| Verify read at 60 MHz | 7.4 s |

Reads at 15 and 60 MHz were byte-identical (SHA-256) to a dump of the same
chip made with a CH341A.

16 MB writes at 60 MHz, after the chip erase, as each optimization landed:

| Driver | Image | Write time |
|---|---|---|
| Held writes, `0xC2` status polls, status polled at once | full-flash dump | 2 min 42 s |
| + chip-select flags (firmware 0341+) | Wyze Pan v3 recovery image | 1 min 26 s |
| + blank-page skip | same recovery image | 46 s |

The recovery image holds 7.3 MB of data: 29,881 of its 65,536 pages are
programmed and 35,655 blank ones are skipped. The log of that run:

```text
17:31:21.922  Found programmer device: WinChipHead (WCH) - CH347T
17:31:21.931  CH347 SPI clock: 15 MHz
17:31:21.932  CH347 firmware version: 0544
17:31:21.932  CH347 chip-select flags: on
17:31:21.935  Detected: NOR: ZB25VQ128 (16.0 MB)
17:31:25.424  CH347 SPI clock: 60 MHz
17:31:33.226  Firmware loaded: recovery_wyzepan3.bin (16.00 MB)
17:31:33.234  Image: data in 137 ranges from 0x000000 to 0xFF00FF (7.3 MB in 29881 of 65536 pages); last data byte at 0xFF000B, then 64.0 KB of blank (0xFF) tail
17:31:43.220  Erasing flash (16.0 MB)...
17:32:14.945  Erase complete
17:32:14.951  Writing recovery_wyzepan3.bin (16.0 MB): programming 29881 pages, skipping 35655 blank (0xFF) pages...
17:32:23.041  Written 10% (1.6 MB / 16.0 MB)
17:32:31.712  Written 20% (3.3 MB / 16.0 MB)
17:32:41.109  Written 30% (4.8 MB / 16.0 MB)
17:32:50.938  Written 40% (6.4 MB / 16.0 MB)
17:32:58.804  Written 50% (8.0 MB / 16.0 MB)
17:32:59.219  Written 60% (9.6 MB / 16.0 MB)
17:32:59.620  Written 70% (11.3 MB / 16.0 MB)
17:33:00.009  Written 80% (12.8 MB / 16.0 MB)
17:33:00.420  Written 90% (14.4 MB / 16.0 MB)
17:33:00.820  Written 100% (16.0 MB / 16.0 MB)
17:33:00.830  Write complete
17:33:08.037  Verify: OK - data matches
```

---

Scriba began as a streamlined fork of [SNANDer](https://github.com/McMCCRU/SNANDer)
by McMCC, later modified by Droid-MAX, and has since been heavily reworked by
[Paul Philippov](https://github.com/themactep) with EZP2019 support, code
optimizations, and structural improvements.
