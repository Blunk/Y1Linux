"""The Innioasis Y1's update service in the normal system (docs/planning/Y1-RECOVERY-PLAN.md, R4b): the Y2's
service daemon in its network and update parts, started by the Y1's S42y2-readiness only on the Y1's own Y2DATA;
S03y1-data records that device as the Y2's initramfs does; the capabilities report OTA off until R5."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from tools.build import y1_capabilities

ROOT = Path(__file__).resolve().parents[1]
S42 = ROOT / 'initramfs/y1-update/etc/init.d/S42y2-readiness'
POLICY = ROOT / 'initramfs/y1-update/etc/y2linux/service-policy.json'
LISTING = (ROOT / 'initramfs/y1-production.list').read_text()
PID1 = (ROOT / 'initramfs/y1-system.h').read_text()


def services():
    text = (ROOT / 'initramfs/y1-bluetooth/etc/init.d/y1-services').read_text()
    return text.split('Y1_SERVICES="')[1].split('"')[0].split()


class Y1UpdateService(unittest.TestCase):
    def test_the_policy_names_the_network_and_update_parts(self):
        self.assertEqual(json.loads(POLICY.read_text()), {'schema': 1, 'network_only': True, 'updates': True})
        daemon = (ROOT / 'platform/crates/y2-platform/src/commands/service/daemon.rs').read_text()
        self.assertIn('const POLICY: &str = "/etc/y2linux/service-policy.json";', daemon)
        self.assertIn('updates: schema && policy.get("updates") == Some(&Value::Bool(true)),', daemon)

    def test_pid1_installs_s42_the_policy_and_the_capabilities(self):
        for path, mode, source in [
                ('/etc/y2linux/service-policy.json', '0644', '/project/initramfs/y1-update/etc/y2linux/service-policy.json'),
                ('/etc/init.d/S42y2-readiness', '0755', '/project/initramfs/y1-update/etc/init.d/S42y2-readiness'),
                ('/etc/y2linux/capabilities.json', '0644', '/build/y1-capabilities.json')]:
            self.assertIn(f'file /y1-system{path} {source} {mode} 0 0\n', LISTING)
            self.assertIn(f'{{ "{path}", {mode} }},', PID1)
        self.assertTrue(os.access(S42, os.X_OK))
        build = (ROOT / 'tools/build/y1-build.sh').read_text()
        self.assertIn('python3 -m tools.build.y1_capabilities "$1/buildroot/target" /build/y1-capabilities.json',
                      build)
        self.assertLess(build.index('y1_capabilities'), build.index('allnoconfig'))

    def test_s42_runs_after_data_power_and_wifi(self):
        listed = services()
        self.assertIn('S42y2-readiness', listed)
        for before in ('S03y1-data', 'S04y2-power', 'S41y2-connectivity'):
            self.assertLess(listed.index(before), listed.index('S42y2-readiness'))
        self.assertLess(listed.index('S42y2-readiness'), listed.index('S90y1-reborn'))

    def run_s42(self, data_device, mounts, platform=b'.../etc/y2linux/service-policy.json...', pending=False):
        """S42 start with its paths moved below a temporary root and start-stop-daemon recorded."""
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            (base / 'bin').mkdir()
            (base / 'run').mkdir()
            (base / 'proc').mkdir()
            (base / 'data/system/platform').mkdir(parents=True)
            (base / 'usr/sbin').mkdir(parents=True)
            (base / 'usr/sbin/y2-platform').write_bytes(platform)
            (base / 'proc/mounts').write_text(mounts)
            if data_device is not None:
                (base / 'run/y2-data-device').write_text(data_device + '\n')
            if pending:
                (base / 'data/system/platform/maintenance-pending').write_text('')
            log = base / 'calls'
            (base / 'bin/start-stop-daemon').write_text(f'#!/bin/sh\necho "$*" >> {log}\n')
            (base / 'bin/start-stop-daemon').chmod(0o755)
            script = S42.read_text()
            for path in ('/run/', '/proc/mounts', '/data/', '/usr/sbin/y2-platform'):
                script = script.replace(path, str(base) + path)
            result = subprocess.run(['sh', '-c', script, 'S42y2-readiness', 'start'], capture_output=True,
                                    text=True, env={'PATH': f'{base}/bin:/usr/bin:/bin'})
            calls = log.read_text() if log.exists() else ''
            return result.returncode, calls

    def test_s42_starts_the_supervisor_only_on_the_y1s_own_data(self):
        data = '/dev/loop1 /data ext4 rw,nosuid,nodev,noatime 0 0\n'
        code, calls = self.run_s42('/dev/loop1', data)
        self.assertEqual(code, 0)
        self.assertIn('-S -q -b -m -p', calls)
        self.assertIn('-x /usr/libexec/y2/readiness-supervise', calls)
        # Reborn's RAM /data (no recorded device, or another one), no /data, a platform before the policy: nothing.
        for device, mounts, platform in [(None, data, None), ('/dev/loop1', '/dev/loop2 /data ext4 rw 0 0\n', None),
                                         ('/dev/loop1', '', None), ('/dev/loop1', data, b'an older platform')]:
            arguments = (device, mounts) + ((platform,) if platform else ())
            self.assertEqual(self.run_s42(*arguments), (0, ''), (device, mounts, platform))
        # A reset in progress: refused, as the Y2's S42.
        self.assertEqual(self.run_s42('/dev/loop1', data, pending=True), (1, ''))

    def test_s03_records_the_data_loop_and_the_updates_directory(self):
        script = (ROOT / 'initramfs/y1-data/etc/init.d/S03y1-data').read_text()
        body = script[script.index('mount_data() {'):script.index('\n}\n', script.index('mount_data() {'))]
        self.assertIn('mkdir -p -m 0700 /data/music /data/reborn /data/system/platform /data/updates', body)
        # Only after the mount and the schema check.
        self.assertLess(body.index('mount -t ext4'), body.index('printf \'%s\\n\' "$dev" > /run/y2-data-device'))
        self.assertLess(body.index('.y2data-schema'), body.index('/run/y2-data-device'))

    def test_the_capabilities_report_ota_off_and_change_nothing_else(self):
        source = json.loads((ROOT / 'platform/capabilities.json').read_text())
        derived = y1_capabilities.derive(source)
        for name in y1_capabilities.OFF:
            self.assertIs(derived['capabilities'][name]['enabled'], False)
            self.assertIn('R5', derived['capabilities'][name]['reason'])
            derived['capabilities'][name].update(enabled=source['capabilities'][name]['enabled'],
                                                 reason=source['capabilities'][name]['reason'])
        self.assertEqual(derived, source)
        self.assertIs(source['capabilities']['ota']['enabled'], True)  # the source is left alone
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp) / 'target'
            (target / 'etc/y2linux').mkdir(parents=True)
            (target / y1_capabilities.SOURCE).write_text(json.dumps(source))
            text = y1_capabilities.build(target, Path(tmp) / 'out.json')
            self.assertLess(len(text.encode()), 65536)
            del source['capabilities']['ota']
            (target / y1_capabilities.SOURCE).write_text(json.dumps(source))
            with self.assertRaises(ValueError):
                y1_capabilities.build(target, Path(tmp) / 'out.json')


if __name__ == '__main__':
    unittest.main()
