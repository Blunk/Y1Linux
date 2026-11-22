"""The Y1's release build (tools/production/y1_release.py): the key it signs with, the trust stores the signed
manifest must verify against, the sequence range, the firmware scan and the first-install zip."""
import json
import os
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch

from tools.production import release, y1_release
from tools.update import package


def trust_store(public_hex, key_id='y1linux-release-test', minimum=16000):
    return {'schema': 1, 'keys': [{'id': key_id, 'purpose': 'release', 'status': 'active',
                                    'public_key_hex': public_hex, 'minimum_sequence': minimum,
                                    'maximum_sequence': 2147483647, 'allow_downgrade': False}]}


class Y1Release(unittest.TestCase):
    def setUp(self):
        self.folder = Path(tempfile.mkdtemp(prefix='y1-release-test-'))
        self.addCleanup(lambda: __import__('shutil').rmtree(self.folder))
        self.key = self.folder / 'signing/test.key'
        self.public = package.keygen(self.key)
        self.trust = self.folder / 'trust.json'
        self.trust.write_text(json.dumps(trust_store(self.public)))

    def test_the_branch_trusts_the_key_release_py_signs_with(self):
        trust = json.loads(y1_release.TRUST.read_text())
        self.assertEqual([key['id'] for key in trust['keys']], [release.KEY_ID])
        self.assertTrue(y1_release.release_key(trust, release.KEY_ID)['public_key_hex'])
        self.assertTrue(release.KEY_ID.startswith('y1linux-release-'))
        self.assertIn(release.KEY_ID, str(release.KEY))

    def test_only_the_trusted_private_key_signs(self):
        with patch.object(y1_release, 'TRUST', self.trust):
            y1_release.check_key(self.key, 'y1linux-release-test')
            other = self.folder / 'signing/other.key'
            package.keygen(other)
            with self.assertRaisesRegex(SystemExit, 'is not y1linux-release-test'):
                y1_release.check_key(other, 'y1linux-release-test')
            with self.assertRaisesRegex(SystemExit, 'no single active release key'):
                y1_release.check_key(self.key, 'y1linux-release-gone')
            os.chmod(self.key, 0o644)
            with self.assertRaisesRegex(SystemExit, 'mode-0600'):
                y1_release.check_key(self.key, 'y1linux-release-test')
            with self.assertRaisesRegex(SystemExit, 'No such file'):
                y1_release.check_key(self.folder / 'lost.key', 'y1linux-release-test')

    def test_the_manifest_verifies_only_with_its_trust_store(self):
        manifest = b'{"sequence":17400}'
        signature = package.sign(manifest, self.key)
        store = trust_store(self.public)
        self.assertTrue(y1_release.verify(manifest, signature, store, 'y1linux-release-test'))
        self.assertFalse(y1_release.verify(manifest + b' ', signature, store, 'y1linux-release-test'))
        stranger = trust_store(package.keygen(self.folder / 'signing/stranger.key'))
        self.assertFalse(y1_release.verify(manifest, signature, stranger, 'y1linux-release-test'))
        self.assertFalse(y1_release.verify(manifest, signature[:63], store, 'y1linux-release-test'))

    def test_a_y1_build_id_and_a_sequence_the_key_accepts(self):
        store = trust_store(self.public, minimum=16000)
        good = {'build_id': 'Y1LINUX-1.0.0-BETA-9.3.36', 'update_sequence': 17400}
        y1_release.check_sequence(good, store, 'y1linux-release-test')
        with self.assertRaisesRegex(SystemExit, 'not a Y1 one'):
            y1_release.check_sequence(dict(good, build_id='Y2LINUX-1.0.0-BETA-9.3.36'), store, 'y1linux-release-test')
        with self.assertRaisesRegex(SystemExit, 'outside'):
            y1_release.check_sequence(dict(good, update_sequence=15999), store, 'y1linux-release-test')

    def test_the_firmware_scan_finds_the_players_firmware_also_gzipped(self):
        firmware = bytes(range(256)) * 40
        clean, dirty, packed = (self.folder / name for name in ('clean.img', 'dirty.img', 'packed.img'))
        clean.write_bytes(b'\0' * 20000)
        dirty.write_bytes(b'\0' * 100 + firmware)
        packed.write_bytes(b'\0' * 100 + __import__('gzip').compress(firmware))
        self.assertEqual(release.firmware_free([clean, dirty, packed], {'WIFI_RAM_CODE_MT6582': firmware}),
                         {'clean.img': False, 'dirty.img': True, 'packed.img': True})
        with self.assertRaisesRegex(SystemExit, 'no probes'):
            release.firmware_free([clean], {'WMT_SOC.cfg': b'small'})
        (self.folder / 'empty').mkdir()
        with self.assertRaisesRegex(SystemExit, 'copy the player'):
            y1_release.firmware_files(self.folder / 'empty')

    def test_the_first_install_zip_has_the_images_their_sums_and_the_steps(self):
        images = {}
        for name in ('rootfs.ext4', 'BOOTIMG.img', 'RECOVERY.img'):
            images[name] = self.folder / ('source-' + name)
            images[name].write_bytes(name.encode() * 100)
        assets = self.folder / 'assets'
        assets.mkdir()
        y1_release.first_install(self.folder, assets, '1.0.0-beta.9.3.36', images)
        with zipfile.ZipFile(assets / 'Y1Linux-1.0.0-beta.9.3.36-first-install.zip') as archive:
            prefix = 'Y1Linux-1.0.0-beta.9.3.36-first-install/'
            self.assertEqual(sorted(name.removeprefix(prefix) for name in archive.namelist() if not name.endswith('/')),
                             ['BOOTIMG.img', 'README.txt', 'RECOVERY.img', 'SHA256SUMS', 'rootfs.ext4'])
            sums = archive.read(prefix + 'SHA256SUMS').decode()
            readme = archive.read(prefix + 'README.txt').decode()
        for name, path in images.items():
            self.assertIn(f'{release.sha256(path)}  {name}\n', sums)
        self.assertIn('blob/y1linux-1.0.0-beta.9.3.36/docs/INSTALL.md', readme)
        self.assertIn('mtk.py w RECOVERY RECOVERY.img', readme)
        self.assertFalse((self.folder / 'Y1Linux-1.0.0-beta.9.3.36-first-install').exists())

    def test_publish_uses_the_y1_tag_and_the_current_branch(self):
        text = Path(release.__file__).read_text()
        body = text[text.index('def publish('):text.index('def main(')]
        self.assertIn("summary.get('tag', f'baseline-{args.label}')", body)
        self.assertIn("git(repo, 'branch', '--show-current')", body)
        self.assertIn("'--latest'", body)
        self.assertNotIn('--prerelease', body)


if __name__ == '__main__':
    unittest.main()
