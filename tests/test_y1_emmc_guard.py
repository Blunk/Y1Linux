"""The Innioasis Y1 eMMC write guard (kernel/y1/emmc-guard.h, docs/planning/Y1-DATA-PLAN.md) and its two policies
(docs/planning/Y1-RECOVERY-PLAN.md, R2)."""
import ctypes, re, subprocess, tempfile, unittest
from pathlib import Path
from tools.build import y1_package
ROOT = Path(__file__).resolve().parents[1]
WINDOWS = 4                       # Y1_EMMC_WINDOWS
FIELDS = ('opcode', 'arg', 'write', 'blocks', 'blksz', 'has_data', 'has_sbc', 'sbc_arg', 'partition',
          'part_config', 'blockaddr', 'window_count')
DISK = 7569408
# The user area from the 3.0.2 scatter (MT6572_Android_scatter.txt): name, scatter address, bytes. Linux's sector =
# (scatter - 0x600000) / 512; FAT runs to the end of the disk.
SCATTER = [('PRO_INFO', 0x1500000, 0x300000), ('NVRAM', 0x1800000, 0x500000), ('PROTECT_F', 0x1d00000, 0xa00000),
           ('PROTECT_S', 0x2700000, 0xa00000), ('SECCFG', 0x3100000, 0x20000), ('UBOOT', 0x3120000, 0x60000),
           ('BOOTIMG', 0x3180000, 0x600000), ('RECOVERY', 0x3780000, 0x600000), ('SEC_RO', 0x3d80000, 0x40000),
           ('MISC', 0x3dc0000, 0x80000), ('LOGO', 0x3e40000, 0x300000), ('EXPDB', 0x4140000, 0xa00000),
           ('ANDROID', 0x4b40000, 0x28a00000), ('CACHE', 0x2d540000, 0x20000000),
           ('USRDATA', 0x4d540000, 0x80000000), ('FAT', 0xcd540000, (DISK * 512 + 0x600000) - 0xcd540000)]
PART = {name: ((address - 0x600000) // 512, size // 512) for name, address, size in SCATTER}
# MBR and EBR1 lie in the sectors before PRO_INFO; the preloader is in boot0.
PART['MBR+EBR1'] = (0x7000, 0x800)
START, SECTORS = PART['USRDATA']
POLICY = {'normal': ['USRDATA', 'RECOVERY', 'MISC'], 'recovery': ['USRDATA', 'BOOTIMG', 'ANDROID', 'MISC']}


class Request(ctypes.Structure):
    _fields_ = [(name, ctypes.c_uint) for name in FIELDS] + [('windows', (ctypes.c_uint * 2) * WINDOWS)]


def windows(names):
    return [PART[name] for name in names]


class Y1EmmcGuard(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        p = Path(cls.tmp.name)
        (p / 'test.c').write_text('#include "emmc-guard.h"\n'
                                  'int allow(const struct y1_emmc_request *r){return y1_emmc_request_allowed(r);}\n'
                                  'int valid(const unsigned *c, unsigned n){return y1_emmc_windows_valid(c, n);}\n'
                                  'unsigned size(void){return sizeof(struct y1_emmc_request);}\n')
        subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC', '-I' + str(ROOT / 'kernel/y1'),
                        str(p / 'test.c'), '-o', str(p / 'test.so')], check=True)
        cls.lib = ctypes.CDLL(str(p / 'test.so'))
        cls.lib.allow.argtypes = [ctypes.POINTER(Request)]
        cls.lib.valid.argtypes = [ctypes.POINTER(ctypes.c_uint), ctypes.c_uint]

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def allowed(self, list_=((START, SECTORS),), **changes):
        fields = dict(blockaddr=1, window_count=len(list_))
        fields.update(changes)
        request = Request(**fields)
        for i, (start, sectors) in enumerate(list_[:WINDOWS]):
            request.windows[i][0], request.windows[i][1] = start, sectors
        return bool(self.lib.allow(ctypes.byref(request)))

    def write(self, arg, blocks=8, **changes):
        fields = dict(opcode=25 if blocks > 1 else 24, arg=arg, write=1, has_data=1, blocks=blocks, blksz=512)
        fields.update(changes)
        return self.allowed(**fields)

    def valid(self, cells):
        return bool(self.lib.valid((ctypes.c_uint * max(len(cells), 1))(*cells), len(cells)))

    def test_layout(self):
        self.assertEqual(self.lib.size(), ctypes.sizeof(Request))
        ordered = sorted(PART.values())
        for (start, sectors), (following, _) in zip(ordered, ordered[1:]):
            self.assertLessEqual(start + sectors, following)
        self.assertEqual(sum(PART['FAT']), DISK)
        self.assertEqual(PART['BOOTIMG'], (0x15c00, 0x3000))
        self.assertEqual(PART['RECOVERY'], (0x18c00, 0x3000))
        self.assertEqual(PART['MISC'], (0x1be00, 0x400))
        self.assertEqual(PART['ANDROID'], (0x22a00, 0x145000))
        self.assertEqual(PART['USRDATA'], (0x267a00, 0x400000))

    def test_writes_only_inside_usrdata(self):
        for blocks in (1, 8, 128, 65535, SECTORS):
            self.assertTrue(self.write(START, blocks))
            self.assertTrue(self.write(START + SECTORS - blocks, blocks))
            self.assertFalse(self.write(START - 1, blocks))
            self.assertFalse(self.write(START + SECTORS - blocks + 1, blocks))
        for arg in (0, 0x22a00, 0x167a00, START + SECTORS, DISK - 8, 0xffffffff):
            self.assertFalse(self.write(arg))           # boot, ANDROID (Y2ROOT), CACHE, FAT, tail, overflow
        self.assertFalse(self.write(START, 0))
        self.assertFalse(self.write(START, 0, opcode=25))
        self.assertFalse(self.write(0xfffffff8, 16))    # arg + blocks wraps
        self.assertFalse(self.write(START, SECTORS + 1))

    def test_writes_need_the_user_area_and_sector_addressing(self):
        for partition in (1, 2, 3):                     # boot0, boot1, RPMB
            self.assertFalse(self.write(START, partition=partition))
        self.assertFalse(self.write(START, blockaddr=0))
        self.assertFalse(self.write(START, blksz=1024))
        self.assertFalse(self.write(START, opcode=24))   # CMD24 is one block
        self.assertFalse(self.allowed(opcode=19, write=1, has_data=1, blocks=1, blksz=512, arg=START))
        self.assertFalse(self.write(START, list_=((START, 0),)))

    def test_no_window_no_write(self):
        self.assertFalse(self.write(START, list_=()))
        # A count above the array is read only as far as the array goes.
        self.assertTrue(self.write(START, window_count=WINDOWS + 1))
        self.assertFalse(self.write(PART['MISC'][0], window_count=WINDOWS + 1))

    def check_policy(self, names):
        """Every window exactly its partition: its first and last sectors and the whole partition writable, a
        sector past either end and a write across into a neighbour refused; every other partition refused at its
        first, middle and last sector."""
        list_ = windows(names)
        for start, sectors in list_:
            for arg, blocks in ((start, 1), (start + sectors - 1, 1), (start, sectors), (start, min(sectors, 256))):
                self.assertTrue(self.write(arg, blocks, list_=list_), (names, hex(arg), blocks))
            for arg, blocks in ((start - 1, 1), (start + sectors, 1), (start - 1, 2), (start + sectors - 1, 2),
                                (start, sectors + 1)):
                if not any(s <= arg and arg + blocks <= s + n for s, n in list_):
                    self.assertFalse(self.write(arg, blocks, list_=list_), (names, hex(arg), blocks))
        for name, (start, sectors) in PART.items():
            if name in names:
                continue
            for arg in (start, start + sectors // 2, start + sectors - 1):
                self.assertFalse(self.write(arg, 1, list_=list_), (names, name, hex(arg)))

    def test_normal_policy(self):
        self.check_policy(POLICY['normal'])
        list_ = windows(POLICY['normal'])
        for name in ('BOOTIMG', 'ANDROID'):             # never the running image or its root
            start, sectors = PART[name]
            self.assertFalse(self.write(start, sectors, list_=list_))
        # BOOTIMG ends where RECOVERY starts: a write over the boundary is refused.
        self.assertFalse(self.write(PART['RECOVERY'][0] - 1, 2, list_=list_))

    def test_recovery_policy(self):
        self.check_policy(POLICY['recovery'])
        list_ = windows(POLICY['recovery'])
        start, sectors = PART['RECOVERY']              # never itself, nor from BOOTIMG across into it
        self.assertFalse(self.write(start, sectors, list_=list_))
        self.assertFalse(self.write(start - 1, 2, list_=list_))
        self.assertTrue(self.write(start - 1, 1, list_=list_))

    def test_policies_leave_the_loaders_and_tables_alone(self):
        for names in POLICY.values():
            for name in ('MBR+EBR1', 'PRO_INFO', 'NVRAM', 'PROTECT_F', 'PROTECT_S', 'SECCFG', 'UBOOT', 'SEC_RO',
                         'LOGO', 'EXPDB', 'CACHE', 'FAT'):
                self.assertNotIn(name, names)
            self.assertLessEqual(len(names), WINDOWS)
            self.assertEqual(names[0], 'USRDATA')         # PID1 attaches Y2DATA only behind this
        self.assertNotIn('BOOTIMG', POLICY['normal'])
        self.assertNotIn('ANDROID', POLICY['normal'])
        self.assertNotIn('RECOVERY', POLICY['recovery'])

    def test_device_trees_carry_the_policies(self):
        source = (ROOT / 'kernel/dts/innioasis-y1.dts').read_text()
        found = re.findall(r'#ifdef Y1_RECOVERY\s+innioasis,emmc-write-window = <([^>]*)>;\s+#else\s+'
                           r'innioasis,emmc-write-window = <([^>]*)>;\s+#endif', source)
        self.assertEqual(len(found), 1)
        self.assertEqual(source.count('innioasis,emmc-write-window'), 2)
        for mode, cells in zip(('recovery', 'normal'), found[0]):
            values = [int(cell, 16) for cell in cells.split()]
            self.assertEqual([tuple(values[i:i + 2]) for i in range(0, len(values), 2)], windows(POLICY[mode]))
            self.assertTrue(self.valid(values))
            dtb = y1_package.WINDOWS['y1-recovery.dtb' if mode == 'recovery' else 'y1.dtb']
            self.assertEqual(dtb, 'innioasis,emmc-write-window = <' + ' '.join(hex(v) for v in values) + '>;')

    def test_window_list_validation(self):
        self.assertTrue(self.valid([START, SECTORS]))
        self.assertTrue(self.valid([0x15c00, 0x3000, 0x18c00, 0x3000]))          # adjacent
        self.assertTrue(self.valid([0, 1, 0xffffffff, 1]))                        # ends exactly at 2^32
        self.assertTrue(self.valid([1, 1, 2, 1, 3, 1, 4, 1]))
        self.assertFalse(self.valid([]))
        self.assertFalse(self.valid([START]))
        self.assertFalse(self.valid([1, 1, 2, 1, 3, 1, 4, 1, 5, 1]))              # five windows
        self.assertFalse(self.valid([START, 0]))
        self.assertFalse(self.valid([0xffffffff, 2]))                             # past the sector range
        self.assertFalse(self.valid([0x15c00, 0x3001, 0x18c00, 0x3000]))          # overlap by one
        self.assertFalse(self.valid([0x18c00, 0x3000, 0x15c00, 0x3001]))          # in either order
        self.assertFalse(self.valid([START, SECTORS, START + 8, 8]))              # one inside another
        self.assertFalse(self.valid([0xfffffff0, 0x10, 0xfffffff8, 1]))           # overlap at the top

    def test_block_count_companion(self):
        for flags in (0, 0x80000000, 0x20000000, 0xa0000000):
            self.assertTrue(self.write(START, 8, has_sbc=1, sbc_arg=flags | 8))
        for arg in (7, 9, 0x40000008, 0x10000008, 0xffffffff):
            self.assertFalse(self.write(START, 8, has_sbc=1, sbc_arg=arg))

    def test_reads_anywhere(self):
        for opcode, blocks in ((17, 1), (18, 8)):
            for arg in (0, START - 1, DISK - blocks):
                self.assertTrue(self.allowed(opcode=opcode, arg=arg, has_data=1, blocks=blocks, blksz=512,
                                             partition=1))
        self.assertTrue(self.allowed(opcode=8, has_data=1, blocks=1, blksz=512))
        self.assertFalse(self.allowed(opcode=56, has_data=1, blocks=1, blksz=512))

    def test_commands_without_data(self):
        for opcode in (1, 2, 3, 5, 7, 9, 10, 12, 13):
            self.assertTrue(self.allowed(opcode=opcode))
        self.assertTrue(self.allowed(opcode=0))
        self.assertTrue(self.allowed(opcode=16, arg=512))
        self.assertFalse(self.allowed(opcode=16, arg=1024))
        for arg in (0xf0f0f0f0, 0xfffffffa):           # pre-idle, boot initiation
            self.assertFalse(self.allowed(opcode=0, arg=arg))
        # Erase/trim/discard, write protect, lock, CID/CSD programming, gen cmd, FFU/vendor, SDIO/SD probes.
        for opcode in (26, 27, 28, 29, 30, 31, 35, 36, 38, 42, 49, 52, 53, 55, 56, 60, 62):
            self.assertFalse(self.allowed(opcode=opcode, arg=START))

    def switch(self, index, value, set_=1, part_config=0):
        return self.allowed(opcode=6, arg=0x03000000 | index << 16 | value << 8 | set_, part_config=part_config)

    def test_ext_csd_switches(self):
        for index, value in ((32, 1), (33, 0), (33, 1), (34, 1), (34, 3), (161, 1), (175, 1), (183, 0),
                             (183, 2), (183, 6), (185, 1), (187, 0x22)):
            self.assertTrue(self.switch(index, value))
        for index, value in ((32, 0), (34, 0), (34, 4), (183, 3), (185, 3), (185, 0x12)):
            self.assertFalse(self.switch(index, value))
        # Persistent or one-time fields: BOOT_WP, RST_n, BKOPS_EN, BKOPS_START, PARTITION_SETTING, WR_REL_SET,
        # USER_WP, SANITIZE, FFU, cache flush policy.
        for index in (173, 162, 163, 164, 155, 167, 171, 165, 169, 30):
            self.assertFalse(self.switch(index, 1))
        self.assertFalse(self.allowed(opcode=6, arg=0x01b30100))   # set bits, not write byte
        self.assertFalse(self.allowed(opcode=6, arg=0x03b30109))   # reserved bits

    def test_partition_config_access_bits_only(self):
        for config in (0x00, 0x48):                      # no boot, boot from boot0 with ack
            for access in range(4):
                self.assertTrue(self.switch(179, (config & ~7) | access, part_config=config))
            self.assertFalse(self.switch(179, config ^ 0x08, part_config=config))
            self.assertFalse(self.switch(179, config ^ 0x40, part_config=config))


if __name__ == '__main__':
    unittest.main()
