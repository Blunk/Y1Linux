"""The Y1 installer in the kernel's built-in initramfs (tools/build/y1_installer.py) and the image check that both
images carry it exactly (tools/build/y1_package.py; docs/planning/Y1-RECOVERY-PLAN.md, R4)."""
import stat, tempfile, unittest
from pathlib import Path
from tools.build import y1_installer, y1_package
from tools.build.dev_initramfs import encode
ROOT = Path(__file__).resolve().parents[1]
# A Y2ROOT build with the R3 core, when this checkout has one (the image tests need real ARM binaries).
TARGETS = [t for t in sorted(ROOT.glob('out/*/buildroot/target')) if (t / 'usr/sbin/y1-update-core').is_file()]


def archive(files):
    entries = {name: (stat.S_IFREG | 0o755, data, 0, 0) for name, data in files.items()}
    entries['sbin'] = (stat.S_IFDIR | 0o755, b'', 0, 0)
    return encode(entries)


class Y1Installer(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def build_root(self, listed, built):
        """A build directory: the installer list naming `listed` and the kernel's cpio holding `built`."""
        (self.root / 'installer/files/sbin').mkdir(parents=True)
        lines = []
        for name, data in listed.items():
            source = self.root / 'installer/files' / name
            source.write_bytes(data)
            lines.append(f'file /{name} {source} 0755 0 0')
        (self.root / y1_package.INSTALLER_LIST).write_text('\n'.join(['dir /sbin 0755 0 0'] + lines) + '\n')
        (self.root / 'kernel/usr').mkdir(parents=True)
        (self.root / 'kernel/usr/initramfs_data.cpio').write_bytes(archive(built))

    def test_cpio_files_reads_regular_files_only(self):
        files = {'sbin/y1-update-core': b'core', 'init': b'', 'etc/y2linux/recovery-update': b'1\n'}
        self.assertEqual(y1_package.cpio_files(archive(files)), files)

    def test_installer_carried_exactly(self):
        files = {'sbin/y1-update-core': b'core' * 99, 'sbin/y2-recovery-ui': b'ui'}
        self.build_root(files, {**files, 'init': b'pid1'})
        self.assertEqual(y1_package.check_installer(self.root), 398)

    def test_installer_differs(self):
        self.build_root({'sbin/y1-update-core': b'new'}, {'sbin/y1-update-core': b'old'})
        with self.assertRaisesRegex(ValueError, 'initramfs carries /sbin/y1-update-core'):
            y1_package.check_installer(self.root)

    def test_installer_missing(self):
        self.build_root({'sbin/y1-update-core': b'core', 'sbin/y2-recovery-ui': b'ui'},
                        {'sbin/y1-update-core': b'core'})
        with self.assertRaisesRegex(ValueError, 'initramfs carries /sbin/y2-recovery-ui'):
            y1_package.check_installer(self.root)

    def test_list_without_the_core(self):
        self.build_root({'sbin/y2-recovery-ui': b'ui'}, {'sbin/y2-recovery-ui': b'ui'})
        with self.assertRaisesRegex(ValueError, 'names y1-update-core'):
            y1_package.check_installer(self.root)

    @unittest.skipUnless(TARGETS, 'no Y2ROOT build with y1-update-core in out/')
    def test_list_from_a_y2root_build(self):
        target = TARGETS[-1]
        found = y1_installer.build(target, self.root)
        self.assertEqual(found['sbin/y1-update-core'], (target / 'usr/sbin/y1-update-core').read_bytes())
        self.assertEqual(found['etc/y2linux/recovery-update'], b'1\n')
        self.assertIn('lib/ld-linux-armhf.so.3', found)
        self.assertNotIn('sbin/e2fsck', found)
        listed = [line.split() for line in (self.root / 'y1-installer.list').read_text().splitlines()
                  if line.startswith('file ')]
        self.assertEqual({name.lstrip('/') for _, name, *_ in listed}, set(found))
        for _, name, source, *_ in listed:
            self.assertEqual(Path(source).read_bytes(), found[name.lstrip('/')])

    @unittest.skipUnless(TARGETS, 'no Y2ROOT build with y1-update-core in out/')
    def test_test_fixtures_refused(self):
        target = self.root / 'target'
        for name in [*y1_installer.PROGRAMS.values(), *y1_installer.CONFIG]:
            (target / name).parent.mkdir(parents=True, exist_ok=True)
            (target / name).write_bytes((TARGETS[-1] / name).read_bytes())
        (target / 'usr/sbin/y1-update-core').write_bytes(b'\x7fELF Y2_TEST_ROOT')
        with self.assertRaisesRegex(ValueError, 'no test fixtures'):
            y1_installer.build(target, self.root / 'out')


if __name__ == '__main__':
    unittest.main()
