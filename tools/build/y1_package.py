"""Package and check the Innioasis Y1 BOOTIMG from a fresh build directory; never access a device.

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
# Properties the compiled tree must carry (docs/knowledge/y1-hardware.md), by subsystem.
EXPECTED = [
    # Memory, console and command line.
    'compatible = "innioasis,y1", "mediatek,mt6572";',
    'reg = <0x80000000 0x1e000000 0x9ea00000 0x1200000>;',
    'stdout-path = "serial0:921600n8";',
    'clk_ignore_unused',
    'loglevel=3 vt.global_cursor_default=0',
    'firmware_class.path=/data/firmware',
    # The wheel (I2C1), the PMIC, its keys (a 3 s centre hold is also KEY_POWER) and VGP1.
    'compatible = "innioasis,apt32f-wheel";',
    'reg = <0x51>;',
    'innioasis,zone-input-name = "Y2 navigation buttons";',
    'label = "Y2 navigation buttons";',
    'compatible = "mediatek,mt6323";',
    'compatible = "mediatek,mt6323-keys";',
    'innioasis,hold-power-ms = <0xbb8>;',
    'regulator-name = "vgp1";',
    'regulator-min-microvolt = <0x2dc6c0>;',
    'regulator-max-microvolt = <0x2dc6c0>;',
    # Storage: the eMMC (writes only inside USRDATA) and the microSD card.
    'max-frequency = <0x2faf080>;',
    'innioasis,emmc-write-window = <0x267a00 0x400000>;',
    # USB peripheral, VUSB, the AUXADC and the charger (docs/planning/Y1-CHARGING-PLAN.md).
    'dr_mode = "peripheral";',
    'mediatek,eye-src = <0x04>;',
    'innioasis,force-session-valid;',
    'compatible = "mediatek,mt6323-auxadc";',
    'innioasis,charger-input;',
    'regulator-name = "vusb";',
    'regulator-min-microvolt = <0x325aa0>;',
    'regulator-max-microvolt = <0x325aa0>;',
    'innioasis,pmic-charger-policy;',
    'compatible = "innioasis,y1-usb-input";',
    'compatible = "innioasis,y2-mt6323-charger";',
    'innioasis,usb-budget-source;',
    'innioasis,max-charge-current-microamp = <0x6ddd0>;',
    'compatible = "mediatek,mt6323-pwrc";',
    # Display, backlight and GPU (docs/planning/Y1-DISPLAY-PLAN.md, Y1-BACKLIGHT-PLAN.md, Y1-REBORN-PLAN.md).
    'compatible = "mediatek,mt6572-dsi";',
    'compatible = "innioasis,y1-lk-panel";',
    'remote-endpoint = <',
    'compatible = "mediatek,mt6572-disp-ovl";',
    'compatible = "mediatek,mt6572-disp-rdma";',
    'compatible = "mediatek,mt6572-disp-mutex";',
    'compatible = "innioasis,y2-backlight";',
    'innioasis,isink-mask = <0x03>;',
    'compatible = "mediatek,mt6572-power-controller";',
    'compatible = "arm,mali-400";',
    'assigned-clock-parents = <',
    # Audio: the AFE, the MT6323 codec, the AW87559 speaker amplifier (docs/planning/Y1-AUDIO-PLAN.md).
    'compatible = "innioasis,y1-mt6572-afe";',
    'compatible = "innioasis,y1-mt6323-codec";',
    'compatible = "simple-audio-card";',
    'simple-audio-card,name = "Y2Audio";',
    'simple-audio-card,pin-switches = "Headphone", "Speaker";',
    'compatible = "awinic,aw87559";',
    'reg = <0x58>;',
    'simple-audio-card,aux-devs = <',
    # Connectivity: Bluetooth and Wi-Fi (docs/planning/Y1-BLUETOOTH-PLAN.md, Y1-WIFI-PLAN.md), its reserved
    # memory, VCN18/VCN28 and the PA supplies, MediaTek's CONN_HIF_BASE window and MT_WF_HIF_IRQ_ID.
    'reg = <0x80000000 0x100000>;',
    'no-map;',
    'compatible = "innioasis,y1-mt6572-connectivity";',
    'reg-names = "mcu", "conn-emi", "btif", "tx-dma", "rx-dma", "wifi";',
    'interrupt-names = "btif", "tx-dma", "rx-dma", "wake", "wifi";',
    'reg = <0x18070000 0x1000 0x80000000 0x100000 0x1100e000 0x100 0x11000380 0x80 0x11000400 0x80 '
    '0x180f0000 0x5c>;',
    'interrupts = <0x00 0x34 0x08 0x00 0x29 0x08 0x00 0x2a 0x08 0x00 0x7a 0x08 0x00 0x7b 0x08>;',
    'clock-names = "spm", "btif", "dma";',
    'memory-region = <',
    'regulator-name = "vcn18";',
    'regulator-min-microvolt = <0x1b7740>;',
    'regulator-max-microvolt = <0x1b7740>;',
    'regulator-name = "vcn28";',
    'regulator-min-microvolt = <0x2ab980>;',
    'regulator-max-microvolt = <0x2ab980>;',
    'regulator-name = "vcn33-bt";',
    'regulator-name = "vcn33-wifi";',
]
# LK's frame buffer is replaced by the DRM pipeline; no supply of the connectivity core is boot-on.
FORBIDDEN = ['simple-framebuffer', 'iommus = ', 'regulator-boot-on', 'chip-id', 'innioasis,bring-up']
# UART0, I2C0, I2C1, mmc0, mmc1, USB and its PHY, DSI, BLS and the connectivity core.
ENABLED = 10
# The PMIC's VGP1, VUSB, VCN18, VCN28 and the PA supplies, and the two fixed card-rail descriptions.
REGULATORS = ['regulator-name = "emmc-vcc";', 'regulator-name = "sd-vcc";', 'regulator-name = "vgp1";',
              'regulator-name = "vusb";', 'regulator-name = "vcn18";', 'regulator-name = "vcn28";',
              'regulator-name = "vcn33-bt";', 'regulator-name = "vcn33-wifi";']


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
    missing = [line for line in EXPECTED if line not in source]
    require(not missing, 'DTB lacks: ' + '; '.join(missing))
    present = [text for text in FORBIDDEN if text in source]
    require(not present, 'DTB must not contain: ' + '; '.join(present))
    enabled = [line for line in source.splitlines() if 'status = "okay"' in line]
    require(len(enabled) == ENABLED, f'exactly {ENABLED} enabled optional devices')
    regulators = [line.strip() for line in source.splitlines() if 'regulator-name' in line]
    require(sorted(regulators) == sorted(REGULATORS), 'exactly the admitted regulators')
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
    (root / 'BOOTIMG.img').write_bytes(image)
    (root / 'package.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
