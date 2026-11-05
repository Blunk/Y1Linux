"""The Innioasis Y1 eMMC write guard (kernel/y1/emmc-guard.h, docs/planning/Y1-DATA-PLAN.md)."""
import ctypes, subprocess, tempfile, unittest
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
FIELDS = ('opcode', 'arg', 'write', 'blocks', 'blksz', 'has_data', 'has_sbc', 'sbc_arg', 'partition',
          'part_config', 'blockaddr', 'window_start', 'window_sectors')
START, SECTORS = 0x267a00, 0x400000  # USRDATA: physical 0x4cf40000, 2 GiB
DISK = 7569408


class Request(ctypes.Structure):
    _fields_ = [(name, ctypes.c_uint) for name in FIELDS]


class Y1EmmcGuard(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        p = Path(cls.tmp.name)
        (p / 'test.c').write_text('#include "emmc-guard.h"\n'
                                  'int allow(const struct y1_emmc_request *r){return y1_emmc_request_allowed(r);}\n')
        subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC', '-I' + str(ROOT / 'kernel/y1'),
                        str(p / 'test.c'), '-o', str(p / 'test.so')], check=True)
        cls.lib = ctypes.CDLL(str(p / 'test.so'))
        cls.lib.allow.argtypes = [ctypes.POINTER(Request)]

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def allowed(self, **changes):
        fields = dict(blockaddr=1, window_start=START, window_sectors=SECTORS)
        fields.update(changes)
        return bool(self.lib.allow(ctypes.byref(Request(**fields))))

    def write(self, arg, blocks=8, **changes):
        fields = dict(opcode=25 if blocks > 1 else 24, arg=arg, write=1, has_data=1, blocks=blocks, blksz=512)
        fields.update(changes)
        return self.allowed(**fields)

    def test_writes_only_inside_usrdata(self):
        for blocks in (1, 8, 128, 65535, SECTORS):
            self.assertTrue(self.write(START, blocks))
            self.assertTrue(self.write(START + SECTORS - blocks, blocks))
            self.assertFalse(self.write(START - 1, blocks))
            self.assertFalse(self.write(START + SECTORS - blocks + 1, blocks))
        for arg in (0, 0x22a00, 0x167a00, START + SECTORS, DISK - 8, 0xffffffff):
            self.assertFalse(self.write(arg))           # boot, ANDROID (Y2ROOT), CACHE, FAT, tail, overflow
        self.assertFalse(self.write(START, 0))
        self.assertFalse(self.write(0xfffffff8, 16))    # arg + blocks wraps
        self.assertFalse(self.write(START, SECTORS + 1))

    def test_writes_need_the_user_area_and_sector_addressing(self):
        for partition in (1, 2, 3):                     # boot0, boot1, RPMB
            self.assertFalse(self.write(START, partition=partition))
        self.assertFalse(self.write(START, blockaddr=0))
        self.assertFalse(self.write(START, blksz=1024))
        self.assertFalse(self.write(START, opcode=24))   # CMD24 is one block
        self.assertFalse(self.allowed(opcode=19, write=1, has_data=1, blocks=1, blksz=512, arg=START))
        self.assertFalse(self.write(START, window_sectors=0))

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
