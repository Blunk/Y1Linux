#!/bin/sh
# Innioasis Y1 BOOTIMG (docs/knowledge/y1-hardware.md, docs/INSTALL.md). Runs inside the locked build userspace:
#   python3 tools/build/run.py --manifest kernel/patches-y1/manifest.json --output out/<fresh-dir> \
#       -- sh /project/tools/build/y1-build.sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu
test ! -e /build/kernel || { echo 'a fresh output directory is required' >&2; exit 1; }
fragment=/project/kernel/config/y1-production.config
make -C /src O=/build/kernel KCONFIG_ALLCONFIG="$fragment" allnoconfig
python3 /project/tools/build/check_config.py /build/kernel/.config "$fragment"
clang --target=arm-linux-gnueabi -mcpu=cortex-a7 -marm -mfloat-abi=soft \
    -Os -fomit-frame-pointer -ffreestanding -fno-builtin -fno-stack-protector \
    -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-pie -nostdlib -static \
    -fuse-ld=lld -Wl,--build-id=none,-e,_start,--gc-sections,-z,max-page-size=4096 \
    -Wall -Wextra -Werror /project/initramfs/start.S /project/initramfs/y1-init.c -o /build/y1-init
qemu-arm -cpu cortex-a7 /build/y1-init --selftest
make -C /src O=/build/kernel -j"$(nproc)" zImage
make -C /src O=/build/kernel scripts_dtc
clang -E -nostdinc -undef -D__DTS__ -x assembler-with-cpp -I /src/include \
    -I /src/arch/arm/boot/dts/mediatek /project/kernel/dts/innioasis-y1.dts -o /build/y1.preprocessed.dts
/build/kernel/scripts/dtc/dtc -I dts -O dtb -o /build/y1.dtb /build/y1.preprocessed.dts
# The RECOVERY image's tree: the same source with Y1_RECOVERY (docs/planning/Y1-RECOVERY-PLAN.md).
clang -E -nostdinc -undef -D__DTS__ -DY1_RECOVERY -x assembler-with-cpp -I /src/include \
    -I /src/arch/arm/boot/dts/mediatek /project/kernel/dts/innioasis-y1.dts -o /build/y1-recovery.preprocessed.dts
/build/kernel/scripts/dtc/dtc -I dts -O dtb -o /build/y1-recovery.dtb /build/y1-recovery.preprocessed.dts
cd /project
python3 -m tools.build.y1_package /build
cd /build
cp kernel/.config kernel.config
sha256sum BOOTIMG.img RECOVERY.img zImage-dtb zImage-recovery-dtb y1.dtb y1-recovery.dtb y1-init kernel.config \
    > SHA256SUMS
cat SHA256SUMS
