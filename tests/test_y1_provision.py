"""The Innioasis Y1 profile of tools/connectivity/provision.py (docs/planning/Y1-BLUETOOTH-PLAN.md, B0), with
synthetic files: the Y1's single-part patch ("1319"), Bluetooth only, no Y2 default records; the Y2 profile's
two-part rule unchanged."""
import hashlib, json, tempfile, unittest
from pathlib import Path
from unittest import mock

from tools.connectivity import provision


def patch(platform, info, size=64):
    data = bytearray(b'20130523111343a\n' + platform + bytes.fromhex('8a008a00') + bytes([info, 0, 6, 0]))
    return bytes(data + bytes(size - len(data)))


class Y1Provision(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.source = self.root / 'owner'
        self.source.mkdir()

    def tearDown(self):
        self.tmp.cleanup()

    def inventory(self, files):
        entries = []
        for name, data in files.items():
            (self.source / name).write_bytes(data)
            entries.append({'filename': name, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
        path = self.root / 'inventory.json'
        path.write_text(json.dumps({'files': entries}))
        return path

    def run_profile(self, files, profile, custom=None):
        path = self.inventory(files)
        name = 'Y1_INVENTORY' if profile == 'y1' else 'INVENTORY'
        with mock.patch.object(provision, name, path):
            return provision.provision(self.source, self.root / 'out', custom, profile)

    def test_y1_single_part_patch(self):
        receipt = self.run_profile({'WMT_SOC.cfg': b'coex_wmt_ant_mode=1\n',
                                    'mt6572_82_patch_e1_0_hdr.bin': patch(b'1319', 0x11)}, 'y1')
        self.assertEqual(receipt['profile'], 'y1')
        self.assertEqual(receipt['patch_order'], ['mt6572_82_patch_e1_0_hdr.bin'])
        self.assertTrue((self.root / 'out/mt6572_82_patch_e1_0_hdr.bin').exists())

    def test_y1_refuses_the_y2_patch_and_defaults(self):
        for data in (patch(b'ALPS', 0x21), patch(b'1319', 0x21), patch(b'1319', 0x12), patch(b'ALPS', 0x11)):
            with self.assertRaises(ValueError):
                self.run_profile({'mt6572_82_patch_e1_0_hdr.bin': data}, 'y1')
        with self.assertRaises(ValueError):
            self.run_profile({'WMT_SOC.cfg': b'x'}, 'y1', custom=self.root / 'libcustom_nvram.so')
        with self.assertRaises(ValueError):
            self.run_profile({'WMT_SOC.cfg': b'x'}, 'y3')

    def test_y2_two_part_rule_unchanged(self):
        receipt = self.run_profile({'mt6582_patch_e1_0_hdr.bin': patch(b'ALPS', 0x21),
                                    'mt6572_82_patch_e1_0_hdr.bin': patch(b'ALPS', 0x22)}, 'y2')
        self.assertEqual(receipt['patch_order'], ['mt6582_patch_e1_0_hdr.bin', 'mt6572_82_patch_e1_0_hdr.bin'])
        with self.assertRaises(ValueError):
            self.run_profile({'mt6572_82_patch_e1_0_hdr.bin': patch(b'1319', 0x11)}, 'y2')

    def test_y1_inventory_matches_the_plan(self):
        data = json.loads(provision.Y1_INVENTORY.read_text())
        names = {x['filename']: (x['bytes'], x['sha256']) for x in data['files']}
        self.assertEqual(names['mt6572_82_patch_e1_0_hdr.bin'][0], 39868)
        self.assertEqual(names['WMT_SOC.cfg'][1], '161f18b686e59a763368ffb1d8229f158fa61493ecc3dff5c6ef8c11a7f5f1e0')
        self.assertFalse(any(x['redistribution_permission_established'] for x in data['files']))


if __name__ == '__main__':
    unittest.main()
