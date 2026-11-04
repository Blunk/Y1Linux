"""Package and check the Innioasis Y1 first-boot BOOTIMG from a fresh build directory; never access a device.

Kernel payload: zImage with the Y1 device tree appended (padded to 8 bytes). The initramfs is built into
the kernel, so the separate ramdisk is an empty archive that only keeps LK's loader satisfied.
"""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import struct
import subprocess

from tools.build.bootimage import pack
from tools.validation.bootimg import check as check_bootimg
from tools.validation.d08 import require, up

BOOTIMG_PARTITION = 0x600000  # Y1 PMT: BOOTIMG at 0x3180000, length 0x600000
ZIMAGE_MAGIC = 0x016f2818
DTB_LIMIT = 0x10000
# Every property the first-boot device tree must carry (docs/knowledge/y1-hardware.md).
DTB_EXPECTED = [
    'compatible = "innioasis,y1", "mediatek,mt6572";',
    'reg = <0x80000000 0x1e000000 0x9ea00000 0x1200000>;',
    'framebuffer@9fc00000 {',
    'compatible = "simple-framebuffer";',
    'reg = <0x9fc00000 0x54600>;',
    'width = <0x1e0>;',
    'height = <0x168>;',
    'stride = <0x3c0>;',
    'format = "r5g6b5";',
    'stdout-path = "serial0:921600n8";',
]


def empty_ramdisk():
    """A newc archive holding only its trailer, gzip-compressed without a timestamp."""
    name = b'TRAILER!!!\0'
    fields = [0] * 13
    fields[4] = 1  # nlink
    fields[11] = len(name)
    archive = b'070701' + b''.join(b'%08x' % f for f in fields) + name
    archive += b'\0' * (-len(archive) % 4)
    archive += b'\0' * (-len(archive) % 512)
    return gzip.compress(archive, compresslevel=9, mtime=0)


def check_dtb(root, dtb):
    require(dtb[:4] == b'\xd0\x0d\xfe\xed', 'DTB magic')
    require(len(dtb) <= DTB_LIMIT, 'DTB size')
    source = subprocess.run([str(root / 'kernel/scripts/dtc/dtc'), '-I', 'dtb', '-O', 'dts', '-q',
                             str(root / 'y1.dtb')], capture_output=True, text=True, check=True).stdout
    missing = [line for line in DTB_EXPECTED if line not in source]
    require(not missing, 'DTB lacks: ' + '; '.join(missing))
    enabled = [line for line in source.splitlines() if 'status = "okay"' in line]
    require(len(enabled) == 1, 'exactly one enabled optional device (UART0)')
    return source


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    root = parser.parse_args().root
    zimage = (root / 'kernel/arch/arm/boot/zImage').read_bytes()
    require(struct.unpack_from('<I', zimage, 0x24)[0] == ZIMAGE_MAGIC, 'zImage magic')
    dtb = (root / 'y1.dtb').read_bytes()
    check_dtb(root, dtb)
    kernel = zimage + dtb + b'\0' * (-len(dtb) % 8)
    ramdisk = empty_ramdisk()
    image = pack(kernel, ramdisk)
    read_end = 2048 + up(len(kernel) + 512, 2048) + up(len(ramdisk) + 512, 2048) + 2 * 2048
    result = check_bootimg(image, kernel, ramdisk, {'bootimg_read_end': read_end})
    require(len(image) <= BOOTIMG_PARTITION, 'fits the Y1 BOOTIMG partition')
    result.update({'partition_bytes': BOOTIMG_PARTITION, 'zimage_bytes': len(zimage), 'dtb_bytes': len(dtb),
                   'zimage_dtb_sha256': hashlib.sha256(kernel).hexdigest()})
    (root / 'zImage-dtb').write_bytes(kernel)
    (root / 'BOOTIMG-y1-first-boot.img').write_bytes(image)
    (root / 'y1-first-boot.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
