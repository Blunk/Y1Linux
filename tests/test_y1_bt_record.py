"""The owner-local Y1 BT_Addr extraction (docs/planning/Y1-BLUETOOTH-PLAN.md, B6), on synthetic data only."""
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from tools.connectivity.y1_bt_record import GENERIC_DEFAULT_SHA256, extract, reason, redacted

ROOT = Path(__file__).resolve().parents[1]


class Y1BtRecord(unittest.TestCase):
    def test_rule_matches_the_driver(self):
        good = bytes.fromhex('0a1b2c3d4e5f') + bytes(26)
        self.assertIsNone(reason(good))
        self.assertEqual(reason(good[:31]), 'record size')
        self.assertEqual(reason(bytes.fromhex('01') + good[1:]), 'multicast address')
        self.assertEqual(reason(bytes(32)), 'zero address')
        self.assertEqual(reason(bytes.fromhex('000046657201') + bytes(26)), 'the generic MT6572 default address')
        self.assertEqual(redacted(good), 'xx:xx:xx:xx:4e:5f')
        header = (ROOT / 'kernel/y1/connectivity/y1-conn-policy.h').read_text()
        self.assertEqual(GENERIC_DEFAULT_SHA256, hashlib.sha256(bytes.fromhex('000046657201')).hexdigest())
        self.assertIn('0xd3,0x09,0x7b,0x82', header)

    @unittest.skipUnless(shutil.which('mke2fs') and shutil.which('debugfs'), 'e2fsprogs unavailable')
    def test_extracts_from_a_dump_at_the_partition_offset(self):
        with tempfile.TemporaryDirectory() as directory:
            d = Path(directory)
            tree = d / 'tree/nvram/APCFG/APRDEB'
            tree.mkdir(parents=True)
            record = bytes.fromhex('0a1b2c3d4e5f') + bytes(range(6, 32))
            (tree / 'BT_Addr').write_bytes(record)
            image = d / 'usrdata.img'
            subprocess.run(['mke2fs', '-q', '-t', 'ext4', '-d', str(d / 'tree'), str(image), '4M'],
                           check=True, capture_output=True)
            dump = d / 'emmc_user.bin'
            dump.write_bytes(b'\x5a' * 65536 + image.read_bytes())
            out = d / 'out'
            self.assertEqual(extract(dump, out, offset=65536), record)
            self.assertEqual((out / 'bt-record').read_bytes(), record)
            self.assertEqual((out / 'bt-record').stat().st_mode & 0o777, 0o600)
            self.assertEqual(sorted(p.name for p in out.iterdir()), ['bt-record'])
            # A default record is refused and nothing is written.
            (tree / 'BT_Addr').write_bytes(bytes.fromhex('000046657201') + record[6:])
            image.unlink()
            subprocess.run(['mke2fs', '-q', '-t', 'ext4', '-d', str(d / 'tree'), str(image), '4M'],
                           check=True, capture_output=True)
            dump.write_bytes(b'\x5a' * 65536 + image.read_bytes())
            with self.assertRaisesRegex(ValueError, 'generic MT6572 default'):
                extract(dump, d / 'out2', offset=65536)
            self.assertFalse((d / 'out2/bt-record').exists())


if __name__ == '__main__':
    unittest.main()
