"""Package and check an Innioasis Y1 test BOOTIMG (variant first to thirty-fourth) from a fresh
build directory; never access a device.

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
# Every property all test device trees must carry (docs/knowledge/y1-hardware.md).
DTB_COMMON = [
    'compatible = "innioasis,y1", "mediatek,mt6572";',
    'reg = <0x80000000 0x1e000000 0x9ea00000 0x1200000>;',
    'stdout-path = "serial0:921600n8";',
]
# LK's frame buffer, borrowed until the seventh boot's DRM pipeline replaces it.
SIMPLEFB = [
    'framebuffer@9fc00000 {',
    'compatible = "simple-framebuffer";',
    'reg = <0x9fc00000 0x54600>;',
    'width = <0x1e0>;',
    'height = <0x168>;',
    'stride = <0x3c0>;',
    'format = "r5g6b5";',
]
SECOND = [
    'clk_ignore_unused',
    'compatible = "innioasis,apt32f-wheel";',
    'reg = <0x51>;',
    'compatible = "mediatek,mt6323";',
    'compatible = "mediatek,mt6323-keys";',
    'regulator-name = "vgp1";',
    'regulator-min-microvolt = <0x2dc6c0>;',
    'regulator-max-microvolt = <0x2dc6c0>;',
    'max-frequency = <0x2faf080>;',
]
# Regulators a variant may describe: the PMIC's VGP1 (admission 2026-11-04), VUSB (2026-11-05) and the
# two fixed card-rail descriptions, which switch nothing.
CARD_RAILS = ['regulator-name = "emmc-vcc";', 'regulator-name = "sd-vcc";']
# Per variant: the additional properties, how many optional devices may be enabled, and the regulators.
VARIANTS = {
    'first': {'expected': SIMPLEFB, 'enabled': 1, 'regulators': []},  # UART0
    'second': {  # UART0, I2C1, mmc0, mmc1 (docs/planning/Y1-SECOND-BOOT-PLAN.md)
        'expected': SIMPLEFB + SECOND,
        'enabled': 4,
        'regulators': CARD_RAILS + ['regulator-name = "vgp1";'],
    },
    'third': {  # the second's devices plus the USB controller and its PHY (docs/planning/Y1-THIRD-BOOT-PLAN.md)
        'expected': SIMPLEFB + SECOND + [
            'dr_mode = "peripheral";',
            'mediatek,eye-src = <0x04>;',
            'innioasis,force-session-valid;',
            'compatible = "mediatek,mt6323-auxadc";',
            'innioasis,charger-input;',
            'regulator-name = "vusb";',
            'regulator-min-microvolt = <0x325aa0>;',
            'regulator-max-microvolt = <0x325aa0>;',
        ],
        'enabled': 6,
        'regulators': CARD_RAILS + ['regulator-name = "vgp1";', 'regulator-name = "vusb";'],
    },
}
# Charging stage A (docs/planning/Y1-CHARGING-PLAN.md): the third tree unchanged; debugfs is kernel config.
VARIANTS['fourth'] = VARIANTS['third']
# Charging stage B: the charger node, its USB budget source and the wrapper's charger-block policy.
VARIANTS['fifth'] = dict(VARIANTS['third'], expected=VARIANTS['third']['expected'] + [
    'innioasis,pmic-charger-policy;',
    'compatible = "innioasis,y1-usb-input";',
    'compatible = "innioasis,y2-mt6323-charger";',
    'innioasis,usb-budget-source;',
    'innioasis,max-charge-current-microamp = <0x6ddd0>;',
])
# Display stage A (docs/planning/Y1-DISPLAY-PLAN.md): the fifth tree unchanged; /dev/mem is kernel config.
VARIANTS['sixth'] = VARIANTS['fifth']
# Display stage B: the DRM pipeline replaces simple-framebuffer; DSI and BLS enabled, the LK panel stub.
VARIANTS['seventh'] = dict(VARIANTS['sixth'], enabled=VARIANTS['sixth']['enabled'] + 2,
                           expected=[line for line in VARIANTS['sixth']['expected'] if line not in SIMPLEFB] + [
                               'compatible = "mediatek,mt6572-dsi";',
                               'compatible = "innioasis,y1-lk-panel";',
                               'remote-endpoint = <',
                               'compatible = "mediatek,mt6572-disp-ovl";',
                               'compatible = "mediatek,mt6572-disp-rdma";',
                               'compatible = "mediatek,mt6572-disp-mutex";',
                           ], forbidden=['simple-framebuffer', 'iommus = '])
# Backlight (docs/planning/Y1-BACKLIGHT-PLAN.md): the seventh tree plus ISINK0/1.
VARIANTS['eighth'] = dict(VARIANTS['seventh'], expected=VARIANTS['seventh']['expected'] + [
    'compatible = "innioasis,y2-backlight";',
    'innioasis,isink-mask = <0x03>;',
])
# Root filesystem step 1 (docs/planning/Y1-ROOTFS-PLAN.md): the eighth tree unchanged.
VARIANTS['ninth'] = VARIANTS['eighth']
# The Y2 system, milestone A (docs/planning/Y1-SYSTEM-PLAN.md): the ninth tree unchanged.
VARIANTS['tenth'] = VARIANTS['ninth']
# USB networking and SSH (docs/planning/Y1-NETWORK-PLAN.md): the tenth tree unchanged.
VARIANTS['eleventh'] = VARIANTS['tenth']
# Both CPU cores (docs/planning/Y1-SMP-PLAN.md): the eleventh tree unchanged; SMP is kernel config.
VARIANTS['twelfth'] = VARIANTS['eleventh']
# Headphone audio (docs/planning/Y1-AUDIO-PLAN.md, stage B): the AFE, the MT6323 codec and the card.
VARIANTS['thirteenth'] = dict(VARIANTS['twelfth'], expected=VARIANTS['twelfth']['expected'] + [
    'compatible = "innioasis,y1-mt6572-afe";',
    'compatible = "innioasis,y1-mt6323-codec";',
    'compatible = "simple-audio-card";',
])
# The speaker (docs/planning/Y1-AUDIO-PLAN.md, stage C): I2C0 with the AW87559 enabled.
VARIANTS['fourteenth'] = dict(VARIANTS['thirteenth'], enabled=VARIANTS['thirteenth']['enabled'] + 1,
                              expected=VARIANTS['thirteenth']['expected'] + [
                                  'compatible = "awinic,aw87559";',
                                  'reg = <0x58>;',
                                  'simple-audio-card,aux-devs = <',
                              ])

# Power-off (docs/planning/Y1-POWER-PLAN.md, stage D1): the MT6323 power controller.
VARIANTS['fifteenth'] = dict(VARIANTS['fourteenth'], expected=VARIANTS['fourteenth']['expected'] + [
    'compatible = "mediatek,mt6323-pwrc";',
])

# Power management core (docs/planning/Y1-REBORN-PLAN.md, stage R1a): the fifteenth tree unchanged.
VARIANTS['sixteenth'] = VARIANTS['fifteenth']

# The GPU (docs/planning/Y1-REBORN-PLAN.md, stage R1b): MFG domain only, Lima on WHPLL.
VARIANTS['seventeenth'] = dict(VARIANTS['sixteenth'], expected=VARIANTS['sixteenth']['expected'] + [
    'compatible = "mediatek,mt6572-power-controller";',
    'compatible = "arm,mali-400";',
    'assigned-clock-parents = <',
])

# Reborn's first UI (docs/planning/Y1-REBORN-PLAN.md, stage R2): the wheel zones and the centre as Reborn's
# navigation buttons.
VARIANTS['eighteenth'] = dict(VARIANTS['seventeenth'], expected=VARIANTS['seventeenth']['expected'] + [
    'innioasis,zone-input-name = "Y2 navigation buttons";',
    'label = "Y2 navigation buttons";',
])

# Sound in Reborn (docs/planning/Y1-REBORN-PLAN.md, stage R3): the card and headphone pin under Reborn's names.
VARIANTS['nineteenth'] = dict(VARIANTS['eighteenth'], expected=VARIANTS['eighteenth']['expected'] + [
    'simple-audio-card,name = "Y2Audio";',
    'simple-audio-card,pin-switches = "Headphone", "Speaker";',
])

# The power daemon (docs/planning/Y1-POWER-PLAN.md, D2): the nineteenth tree unchanged.
VARIANTS['twentieth'] = VARIANTS['nineteenth']

# Y2DATA (docs/planning/Y1-DATA-PLAN.md, stage B2): the eMMC write guard's window, USRDATA only.
VARIANTS['twenty-first'] = dict(VARIANTS['twentieth'], expected=VARIANTS['twentieth']['expected'] + [
    'innioasis,emmc-write-window = <0x267a00 0x400000>;',
])

# The configfs USB gadget (docs/planning/Y1-MTP-PLAN.md, stage M1): the twenty-first tree unchanged.
VARIANTS['twenty-second'] = VARIANTS['twenty-first']

# The charge-only mode (docs/planning/Y1-CHARGE-MODE-PLAN.md, C1): the twenty-first tree unchanged.
VARIANTS['twenty-third'] = VARIANTS['twenty-first']

# The configfs gadget retried with the charge-only mode (docs/planning/Y1-MTP-PLAN.md): the twenty-third tree.
VARIANTS['twenty-fourth'] = VARIANTS['twenty-third']

# MTP (docs/planning/Y1-MTP-PLAN.md, M2): the twenty-fourth tree unchanged.
VARIANTS['twenty-fifth'] = VARIANTS['twenty-fourth']

# MTP diagnosis with musb event tracing (docs/planning/Y1-MTP-PLAN.md, X1): the twenty-fourth tree unchanged.
VARIANTS['twenty-sixth'] = VARIANTS['twenty-fourth']

# MTP with the musb RX restart fix (docs/planning/Y1-MTP-PLAN.md, X2): the twenty-fifth tree unchanged.
VARIANTS['twenty-seventh'] = VARIANTS['twenty-fifth']

# The quiet boot with the splash (docs/planning/Y1-BOOT-SPLASH-PLAN.md): the twenty-seventh tree with the Y2's
# console level.
VARIANTS['twenty-eighth'] = dict(VARIANTS['twenty-seventh'], expected=VARIANTS['twenty-seventh']['expected'] + [
    'loglevel=3 vt.global_cursor_default=0',
])

# The Y2's charging screen in the charge-only mode (docs/planning/Y1-CHARGE-MODE-PLAN.md): the twenty-eighth tree.
VARIANTS['twenty-ninth'] = VARIANTS['twenty-eighth']

# Power off from Reborn (docs/planning/Y1-REBORN-PLAN.md): a 3 s centre hold is also KEY_POWER.
VARIANTS['thirtieth'] = dict(VARIANTS['twenty-ninth'], expected=VARIANTS['twenty-ninth']['expected'] + [
    'innioasis,hold-power-ms = <0xbb8>;',
])

# The connectivity core's memory reserved (docs/planning/Y1-BLUETOOTH-PLAN.md, B3).
VARIANTS['thirty-first'] = dict(VARIANTS['thirtieth'], expected=VARIANTS['thirtieth']['expected'] + [
    'reg = <0x80000000 0x100000>;',
    'no-map;',
])

# Bluetooth B4 (docs/planning/Y1-BLUETOOTH-PLAN.md): the connectivity owner limited to the chip-id bring-up, with
# VCN18 and VCN28 (fixed 1.8 V / 2.8 V, off as LK leaves them); never the PA rails in this stage.
VARIANTS['thirty-second'] = dict(
    VARIANTS['thirty-first'], enabled=VARIANTS['thirty-first']['enabled'] + 1,
    expected=VARIANTS['thirty-first']['expected'] + [
        'compatible = "innioasis,y1-mt6572-connectivity";',
        'innioasis,bring-up = "chip-id";',
        'reg-names = "mcu", "conn-emi", "btif", "tx-dma", "rx-dma";',
        'interrupt-names = "btif", "tx-dma", "rx-dma", "wake";',
        'clock-names = "spm", "btif", "dma";',
        'memory-region = <',
        'regulator-name = "vcn18";',
        'regulator-min-microvolt = <0x1b7740>;',
        'regulator-max-microvolt = <0x1b7740>;',
        'regulator-name = "vcn28";',
        'regulator-min-microvolt = <0x2ab980>;',
        'regulator-max-microvolt = <0x2ab980>;',
    ],
    forbidden=VARIANTS['thirty-first'].get('forbidden', []) + ['vcn33', 'regulator-boot-on'],
    regulators=VARIANTS['thirty-first']['regulators'] + ['regulator-name = "vcn18";', 'regulator-name = "vcn28";'])

# Bluetooth B5: the "wmt" bring-up (started from userspace), the PA supplies at 3.3 V (on for the RF calibration
# only) and the firmware path on Y2DATA.
VARIANTS['thirty-third'] = dict(
    VARIANTS['thirty-second'],
    expected=[line for line in VARIANTS['thirty-second']['expected'] if 'chip-id' not in line] + [
        'innioasis,bring-up = "wmt";',
        'firmware_class.path=/data/firmware',
        'regulator-name = "vcn33-bt";',
        'regulator-name = "vcn33-wifi";',
        'regulator-min-microvolt = <0x325aa0>;',
        'regulator-max-microvolt = <0x325aa0>;',
    ],
    forbidden=[t for t in VARIANTS['thirty-second']['forbidden'] if t != 'vcn33'] + ['chip-id'],
    regulators=VARIANTS['thirty-second']['regulators'] + ['regulator-name = "vcn33-bt";',
                                                          'regulator-name = "vcn33-wifi";'])

# Bluetooth B6: the full connectivity driver (no bring-up limit), hci0 for BlueZ; the thirty-third tree otherwise.
VARIANTS['thirty-fourth'] = dict(
    VARIANTS['thirty-third'],
    expected=[line for line in VARIANTS['thirty-third']['expected'] if 'bring-up' not in line],
    forbidden=VARIANTS['thirty-third']['forbidden'] + ['innioasis,bring-up'])


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


def check_dtb(root, dtb, variant):
    require(dtb[:4] == b'\xd0\x0d\xfe\xed', 'DTB magic')
    require(len(dtb) <= DTB_LIMIT, 'DTB size')
    source = subprocess.run([str(root / 'kernel/scripts/dtc/dtc'), '-I', 'dtb', '-O', 'dts', '-q',
                             str(root / 'y1.dtb')], capture_output=True, text=True, check=True).stdout
    rules = VARIANTS[variant]
    missing = [line for line in DTB_COMMON + rules['expected'] if line not in source]
    require(not missing, 'DTB lacks: ' + '; '.join(missing))
    present = [text for text in rules.get('forbidden', []) if text in source]
    require(not present, 'DTB must not contain: ' + '; '.join(present))
    enabled = [line for line in source.splitlines() if 'status = "okay"' in line]
    require(len(enabled) == rules['enabled'], f'exactly {rules["enabled"]} enabled optional devices')
    regulators = [line.strip() for line in source.splitlines() if 'regulator-name' in line]
    require(sorted(regulators) == sorted(rules['regulators']), 'exactly the admitted regulators')
    return source


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('variant', choices=sorted(VARIANTS))
    parser.add_argument('root', type=Path)
    args = parser.parse_args()
    variant, root = args.variant, args.root
    zimage = (root / 'kernel/arch/arm/boot/zImage').read_bytes()
    require(struct.unpack_from('<I', zimage, 0x24)[0] == ZIMAGE_MAGIC, 'zImage magic')
    dtb = (root / 'y1.dtb').read_bytes()
    check_dtb(root, dtb, variant)
    kernel = zimage + dtb + b'\0' * (-len(dtb) % 8)
    ramdisk = empty_ramdisk()
    image = pack(kernel, ramdisk)
    read_end = 2048 + up(len(kernel) + 512, 2048) + up(len(ramdisk) + 512, 2048) + 2 * 2048
    result = check_bootimg(image, kernel, ramdisk, {'bootimg_read_end': read_end})
    require(len(image) <= BOOTIMG_PARTITION, 'fits the Y1 BOOTIMG partition')
    result.update({'partition_bytes': BOOTIMG_PARTITION, 'zimage_bytes': len(zimage), 'dtb_bytes': len(dtb),
                   'zimage_dtb_sha256': hashlib.sha256(kernel).hexdigest()})
    (root / 'zImage-dtb').write_bytes(kernel)
    (root / f'BOOTIMG-y1-{variant}-boot.img').write_bytes(image)
    (root / f'y1-{variant}-boot.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
