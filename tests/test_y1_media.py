"""The Innioasis Y1's built-in microSD as Internal Storage (S03y1-media): the card sits inside the case, so it is
mounted at /media/internal and recorded Ready in /run/y2/internal-media.json as the Y2's platform records Y2Media;
Reborn and MTP (S45y1-mtp) then use /media/internal/music. Without the card everything stays on /data/music."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
S03 = ROOT / 'initramfs/y1-data/etc/init.d/S03y1-media'
S45 = ROOT / 'initramfs/y1-mtp/etc/init.d/S45y1-mtp'
LISTING = (ROOT / 'initramfs/y1-production.list').read_text()
PID1 = (ROOT / 'initramfs/y1-system.h').read_text()
BOOT = '0f0e0d0c-0b0a-4908-8706-050403020100'
CARD = '/sys/devices/platform/11130000.mmc/mmc_host/mmc1/mmc1:aaaa/block/mmcblk1'


def services():
    text = (ROOT / 'initramfs/y1-bluetooth/etc/init.d/y1-services').read_text()
    return text.split('Y1_SERVICES="')[1].split('"')[0].split()


def stub(base, name, body):
    path = base / 'bin' / name
    path.write_text('#!/bin/sh\n' + body)
    path.chmod(0o755)


def fake_root(tmp, controller_path=CARD, filesystem='vfat'):
    """A root below `tmp` with the commands S03 calls recorded: mount appends to mountinfo."""
    base = Path(tmp)
    for d in ('bin', 'run', 'dev', 'media', 'proc/self', 'proc/sys/kernel/random'):
        (base / d).mkdir(parents=True, exist_ok=True)
    (base / 'proc/sys/kernel/random/boot_id').write_text(BOOT + '\n')
    (base / 'proc/self/mountinfo').write_text('26 1 0:19 / / rw - overlay overlay rw\n')
    (base / 'proc/mounts').write_text('')
    (base / 'dev/kmsg').write_text('')
    log = base / 'calls'
    stub(base, 'readlink', f'echo {controller_path}\n')
    stub(base, 'blkid', f'echo "$1: UUID=\\"0CDF-1366\\" TYPE=\\"{filesystem}\\""\n')
    stub(base, 'blockdev', f'echo "blockdev $*" >> {log}\n')
    stub(base, 'mount', f'echo "mount $*" >> {log}\n'
                        f'echo "41 26 179:1 / {base}/media/internal rw - vfat /dev/mmcblk1p1 rw" '
                        f'>> {base}/proc/self/mountinfo\n')
    stub(base, 'umount', f'echo "umount $*" >> {log}\n')
    return base


def run(base, script, *args):
    text = script.read_text()
    for path in ('/run/', '/proc/', '/media/', '/dev/kmsg', '/data/'):
        text = text.replace(path, str(base) + path)
    # The block device test: the card's partition is there.
    text = text.replace('[ -b "$dev" ]', '[ "$dev" = /dev/mmcblk1p1 ]')
    return subprocess.run(['sh', '-c', text, script.name, *args], capture_output=True, text=True,
                          env={'PATH': f'{base}/bin:/usr/bin:/bin'})


class Y1Media(unittest.TestCase):
    def test_s03_runs_after_data_and_before_its_users(self):
        listed = services()
        self.assertLess(listed.index('S03y1-data'), listed.index('S03y1-media'))
        for after in ('S41y2-connectivity', 'S42y2-readiness', 'S45y1-mtp', 'S90y1-reborn'):
            self.assertLess(listed.index('S03y1-media'), listed.index(after))

    def test_pid1_installs_s03(self):
        self.assertIn('file /y1-system/etc/init.d/S03y1-media /project/initramfs/y1-data/etc/init.d/S03y1-media '
                      '0755 0 0\n', LISTING)
        self.assertIn('{ "/etc/init.d/S03y1-media", 0755 },', PID1)
        self.assertTrue(os.access(S03, os.X_OK))

    def test_the_card_becomes_internal_storage(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = fake_root(tmp)
            result = run(base, S03, 'start')
            self.assertIn('OK', result.stdout, result.stderr)
            calls = (base / 'calls').read_text()
            self.assertIn('blockdev --setrw /dev/mmcblk1\nblockdev --setrw /dev/mmcblk1p1\n', calls)
            self.assertIn(f'mount -t vfat -o rw,nosuid,nodev,noexec,noatime,', calls)
            self.assertIn(f'/dev/mmcblk1p1 {base}/media/internal\n', calls)
            self.assertNotIn('mmcblk0', calls)
            record = json.loads((base / 'run/y2/internal-media.json').read_text())
            self.assertEqual(record, {'schema': 1, 'state': 'Ready', 'reason': None, 'path': f'{base}/media/internal',
                                      'boot_id': BOOT, 'mount_id': 41, 'device_id': '179:1'})
            self.assertTrue((base / 'media/internal/music').is_dir())

    def test_no_card_on_its_controller_leaves_internal_storage_on_y2data(self):
        for controller, filesystem in [('/sys/devices/platform/11120000.mmc/block/mmcblk1', 'vfat'),
                                       (CARD, 'ext4')]:
            with tempfile.TemporaryDirectory() as tmp:
                base = fake_root(tmp, controller, filesystem)
                result = run(base, S03, 'start')
                self.assertIn('FAIL, ', result.stdout)
                self.assertFalse((base / 'calls').exists())
                record = json.loads((base / 'run/y2/internal-media.json').read_text())
                self.assertEqual((record['state'], record['reason']), ('Unavailable', 'no_card'))

    def test_mtp_serves_the_card_only_on_its_ready_mount(self):
        chooser = S45.read_text().split('choose_storage() {')[1].split('\n}\n')[0]
        with tempfile.TemporaryDirectory() as tmp:
            base = fake_root(tmp)
            run(base, S03, 'start')

            def chosen():
                script = (f'MUSIC=/data/music\nNAME=Music\nchoose_storage() {{{chooser}\n}}\n'
                          'choose_storage\necho "$NAME=$MUSIC"\n')
                script = script.replace('/run/', f'{base}/run/').replace('/proc/', f'{base}/proc/')
                script = script.replace('/media/', f'{base}/media/').replace(f'{base}{base}', str(base))
                return subprocess.run(['sh', '-c', script], capture_output=True, text=True).stdout.strip()

            self.assertEqual(chosen(), f'Internal Storage={base}/media/internal/music')
            # Another mount (a remount, another boot's record) is not the one recorded.
            info = base / 'proc/self/mountinfo'
            info.write_text(info.read_text().replace('41 26 179:1', '42 26 179:1'))
            self.assertEqual(chosen(), 'Music=/data/music')


if __name__ == '__main__':
    unittest.main()
