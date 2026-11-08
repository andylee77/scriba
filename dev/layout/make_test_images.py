#!/usr/bin/env python3
"""Write synthetic flash images for testing src/flash_layout.c.

Nothing here comes from a real device, so the images can be shared. They
cover the empty, tiny, blank and random cases, odd mtdparts strings, and a
4 MB flash laid out like a camera's: an Ingenic boot header, a U-Boot
environment with a valid CRC holding mtdparts, a uImage with a valid header
CRC, a squashfs superblock and an empty JFFS2 area.

Usage: make_test_images.py OUTDIR
"""
import os
import random
import struct
import sys
import zlib


def write(outdir, name, data):
    with open(os.path.join(outdir, name), "wb") as f:
        f.write(data)


def env_block(size, variables):
    """U-Boot environment: CRC32 of the data, then NUL-separated vars."""
    data = b"\0".join(v.encode() for v in variables) + b"\0\0"
    data = data.ljust(size - 4, b"\0")
    return struct.pack("<I", zlib.crc32(data) & 0xFFFFFFFF) + data


def uimage_header(name, payload_size):
    """Legacy uImage header (big-endian) with a valid header CRC."""
    fields = [0x27051956, 0, 0x5F000000, payload_size, 0x80010000,
              0x80010000, 0, 5, 2, 2, 3]   # os linux, arch arm, kernel, lzma
    hdr = struct.pack(">7I4B", *fields) + name.encode().ljust(32, b"\0")
    hcrc = zlib.crc32(hdr) & 0xFFFFFFFF
    return hdr[:4] + struct.pack(">I", hcrc) + hdr[8:]


def squashfs_superblock(bytes_used):
    sb = bytearray(96)
    sb[0:4] = b"hsqs"
    struct.pack_into("<H", sb, 20, 4)              # compression: xz
    struct.pack_into("<H", sb, 28, 4)              # major version 4
    struct.pack_into("<Q", sb, 40, bytes_used)
    return bytes(sb)


def camera_flash():
    size = 4 * 1024 * 1024
    img = bytearray(b"\xff" * size)
    rng = random.Random(1)

    boot = b"\x06\x05\x04\x03\x02\x55\xaa\x55\xaa" + bytes(rng.getrandbits(8) for _ in range(150 * 1024))
    img[0:len(boot)] = boot

    env = env_block(0x10000, [
        "bootcmd=sf probe;sf read 0x80600000 0x50000 0x180000;bootm 0x80600000",
        "bootargs=console=ttyS1,115200n8 root=/dev/mtdblock3 rootfstype=squashfs "
        "mtdparts=jz_sfc:256k(boot),64k(env),1536k(kernel),1792k(rootfs),-(data)",
        "baudrate=115200",
    ])
    img[0x40000:0x40000 + len(env)] = env

    payload = bytes(rng.getrandbits(8) for _ in range(1200 * 1024))
    kernel = uimage_header("Linux-synthetic", len(payload)) + payload
    img[0x50000:0x50000 + len(kernel)] = kernel

    rootfs = squashfs_superblock(1500 * 1024) + bytes(rng.getrandbits(8) for _ in range(1500 * 1024 - 96))
    img[0x1D0000:0x1D0000 + len(rootfs)] = rootfs

    # data: one JFFS2 clean marker per 64 KB erase block (formatted, empty)
    marker = struct.pack("<HHI", 0x1985, 0x2003, 12) + b"\0" * 4
    for off in range(0x390000, size, 0x10000):
        img[off:off + len(marker)] = marker
    return bytes(img)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    outdir = sys.argv[1]
    os.makedirs(outdir, exist_ok=True)
    rng = random.Random(2)
    write(outdir, "empty.bin", b"")
    write(outdir, "tiny.bin", b"\x06\x05\x04")
    write(outdir, "blank.bin", b"\xff" * (1024 * 1024))
    write(outdir, "random.bin", bytes(rng.getrandbits(8) for _ in range(1024 * 1024)))
    weird = bytearray(rng.getrandbits(8) for _ in range(1024 * 1024))
    weird[0x1000:0x1000] = (b"mtdparts=spi0.0:256k(u-boot)ro,64k@0x40000(env),-(rootfs) "
                            b"mtdparts=${mtdparts} mtdparts=x:1(a)\0")
    write(outdir, "weird_mtd.bin", bytes(weird))
    write(outdir, "camera_4mb.bin", camera_flash())
    print("wrote 6 images to", outdir)


if __name__ == "__main__":
    main()
