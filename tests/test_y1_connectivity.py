"""Innioasis Y1 Bluetooth B2: the MT6572 connectivity owner's own logic, on the host.

docs/planning/Y1-BLUETOOTH-PLAN.md B2. The Y1's facts live in kernel/y1/connectivity/y1-conn-policy.h
(pure C); the Y1's wmt.c functions are compiled here from their actual text with mocks, as
tests/test_connectivity.py does for the Y2. The C cases need clang (the build sandbox has it).
"""
import hashlib
import json
from pathlib import Path
import re
import unittest
from test_power import run_c
from test_audio import function

ROOT = Path(__file__).resolve().parents[1]
Y1 = ROOT / 'kernel/y1/connectivity'
POLICY = '#include "../y1/connectivity/y1-conn-policy.h"\n'
INVENTORY = json.loads((ROOT / 'docs/knowledge/evidence/y1-firmware-inventory.json').read_text())
# The owner's stock WMT_SOC.cfg itself (82 bytes, the inventory's SHA-256): four settings, no secrets.
STOCK_CFG = 'coex_wmt_ant_mode=1\n\nwmt_gps_lna_pin=0\nwmt_gps_lna_enable=0\n\nsdio_driving_cfg=0\n\n\n'


def c_bytes(data):
    return '{' + ','.join(str(b) for b in data) + '}'


def c_string(text):
    escapes = {'\\': '\\\\', '"': '\\"', '\n': '\\n', '\r': '\\r'}
    return '"' + ''.join(escapes.get(ch, ch) for ch in text) + '"'


class Y1ConnectivityFacts(unittest.TestCase):
    def test_stock_config_size_matches_the_inventory(self):
        entry = next(f for f in INVENTORY['files'] if f['filename'] == 'WMT_SOC.cfg')
        self.assertEqual(len(STOCK_CFG), entry['bytes'])
        self.assertEqual(hashlib.sha256(STOCK_CFG.encode()).hexdigest(), entry['sha256'])
        for line in STOCK_CFG.split():
            self.assertIn(line, entry['function'])
        self.assertIn('no co_clock_flag', entry['function'])

    def test_firmware_table_matches_the_inventory_and_has_no_modem_or_wifi(self):
        header = (Y1 / 'conn.h').read_text()
        self.assertIn('#define Y2_CONN_FW "mediatek/mt6572/"', header)
        rows = re.findall(r'\{Y2_CONN_FW "([^"]+)", (\d+), \{([^}]*)\}\}', header)
        table = {name: (int(size), bytes(int(b, 16) for b in digest.split(',')).hex())
                 for name, size, digest in rows}
        wanted = {f['filename']: (f['bytes'], f['sha256']) for f in INVENTORY['files']
                  if f['filename'] != 'modem_1_2g_n.img'}
        self.assertEqual(table, wanted)
        for f in INVENTORY['files']:
            if f['filename'] in table:
                self.assertTrue(f['proposed_install_path'].startswith('/lib/firmware/mediatek/mt6572/'))

    def test_default_address_hash_is_the_stock_mt6572_default(self):
        # libbluetooth_mtk .data 0x4004 (B1 §4); the Y2's hash is the same rule for 65:82.
        header = (Y1 / 'y1-conn-policy.h').read_text()
        block = header[header.index('#define Y1_CONN_DEFAULT_ADDRESS_SHA256'):]
        digest = bytes(int(b, 16) for b in re.findall(r'0x([0-9a-f]{2})', block[:block.index('}')]))
        self.assertEqual(digest, hashlib.sha256(bytes.fromhex('000046657201')).digest())
        y2 = (ROOT / 'kernel/platform/connectivity/conn.h').read_text()
        block = y2[y2.index('#define Y2_CONN_DEFAULT_ADDRESS_SHA256'):]
        digest = bytes(int(b, 16) for b in re.findall(r'0x([0-9a-f]{2})', block[:block.index('}')]))
        self.assertEqual(digest, hashlib.sha256(bytes.fromhex('000046658201')).digest())

    def test_shared_hci_reads_only_inside_the_32_byte_record(self):
        hci = (ROOT / 'kernel/platform/connectivity/hci.c').read_text()
        uses = re.findall(r'command\(hdev, 0x[0-9a-f]+, (\d+), c->bt_factory \+ (\d+)\)', hci)
        self.assertEqual(sorted((int(o), int(n)) for n, o in uses), [(12, 6), (18, 7), (27, 3)])
        self.assertLessEqual(max(int(o) + int(n) for n, o in uses), 30)
        self.assertIn('unsigned char bt_factory[Y1_CONN_RECORD_SIZE];', (Y1 / 'conn.h').read_text())

    def test_the_y2_sources_are_shared_unchanged_and_pinned(self):
        manifest = json.loads((ROOT / 'kernel/patches-y1/manifest.json').read_text())
        added = {a['path']: a['source'] for a in manifest['added'] if a['path'].startswith('drivers/bluetooth/')}
        shared = {'btif.c', 'stp.c', 'hci.c', 'firmware.c'}
        for name in shared:
            self.assertEqual(added['drivers/bluetooth/y1-conn-' + name], 'kernel/platform/connectivity/' + name)
        self.assertEqual(added['drivers/bluetooth/protocol.h'], 'kernel/platform/connectivity/protocol.h')
        for own in ('core.c', 'wmt.c'):
            self.assertEqual(added['drivers/bluetooth/y1-conn-' + own], 'kernel/y1/connectivity/' + own)
        self.assertEqual(added['drivers/bluetooth/conn.h'], 'kernel/y1/connectivity/conn.h')
        # No Y2 modem, Wi-Fi or SPM code in the Y1 driver.
        self.assertFalse(any(re.search(r'md\.c|wifi|domain\.h|spm', s) for s in added.values()))

    def test_built_not_loaded_no_y1_tree_names_the_driver(self):
        compatible = re.search(r'compatible="([^"]+)"', (Y1 / 'core.c').read_text()).group(1)
        self.assertEqual(compatible, 'innioasis,y1-mt6572-connectivity')
        for tree in sorted((ROOT / 'kernel/dts').glob('*y1*')):
            self.assertNotIn(compatible, tree.read_text(), tree.name)

    def test_never_the_jtag_pads_or_gpio_modes(self):
        # B1 §0: stock writes 0x10208000/0x10005300 only behind its JTAG debug flag.
        for source in sorted(Y1.iterdir()):
            text = source.read_text().lower()
            for forbidden in ('10208', '10005300', '0x5300'):
                self.assertNotIn(forbidden, text, source.name)


class Y1ConnectivityC(unittest.TestCase):
    def test_wiring_table(self):
        run_c(r'''
#include <assert.h>
#include <string.h>
''' + POLICY + r'''
int main(void){
 static const struct y1_conn_range ranges[]={{"mcu",0x18070000,0x1000},{"conn-emi",0x80000000,0x100000},
  {"btif",0x1100e000,0x100},{"tx-dma",0x11000380,0x80},{"rx-dma",0x11000400,0x80}};
 static const struct y1_conn_irq irqs[]={{"btif",52},{"tx-dma",41},{"rx-dma",42},{"wake",122}};
 assert(sizeof(y1_conn_ranges)==sizeof(ranges) && sizeof(y1_conn_irqs)==sizeof(irqs));
 for(unsigned i=0;i<5;i++)
  assert(!strcmp(y1_conn_ranges[i].name,ranges[i].name) && y1_conn_ranges[i].start==ranges[i].start &&
         y1_conn_ranges[i].size==ranges[i].size && y1_conn_ranges[i].start!=0x1100c000);
 for(unsigned i=0;i<4;i++)
  assert(!strcmp(y1_conn_irqs[i].name,irqs[i].name) && y1_conn_irqs[i].spi==irqs[i].spi);
 assert(Y1_CONN_CHIP==0x6572 && Y1_CONN_GIC_SPI==0 && Y1_CONN_LEVEL_LOW==8);
}
''')

    def test_domain_sequences_follow_stock_order(self):
        run_c(r'''
#include <assert.h>
''' + POLICY + r'''
static unsigned ctl,sta,sta2,prot,key,clock_released_reset,delays[8],ndelays;
static unsigned *spm(unsigned reg){
 assert(reg==Y1_SPM_CONN_PWR_CON || reg==Y1_SPM_PWR_STATUS || reg==Y1_SPM_PWR_STATUS_2ND);
 return reg==Y1_SPM_CONN_PWR_CON?&ctl:reg==Y1_SPM_PWR_STATUS?&sta:&sta2;
}
static void settle(void){
 unsigned on=(ctl&(Y1_PWR_ON|Y1_PWR_ON_2ND))==(Y1_PWR_ON|Y1_PWR_ON_2ND);
 sta=sta2=on?Y1_SPM_CONN_STATUS:0;
}
static void run(const struct y1_conn_step *s,unsigned n){
 for(unsigned i=0;i<n;i++,s++){
  unsigned before=ctl;
  switch(s->op){
  case Y1_SPM_WRITE: assert(!s->reg && s->value==0x0b160001 && i==0); key++; break;
  case Y1_SPM_SET: *spm(s->reg)|=s->value; break;
  case Y1_SPM_CLEAR: *spm(s->reg)&=~s->value; break;
  case Y1_SPM_POLL_SET: assert((*spm(s->reg)&s->value)==s->value); break;
  case Y1_SPM_POLL_CLEAR: assert(!(*spm(s->reg)&s->value)); break;
  case Y1_INFRA_SET: assert(s->reg==0x220 && s->value==0x11); prot|=0x11; break;
  case Y1_INFRA_CLEAR: assert(s->reg==0x220 && s->value==0x11); prot&=~0x11u; break;
  case Y1_INFRA_POLL_SET: assert(s->reg==0x228 && prot==0x11); break;
  case Y1_INFRA_POLL_CLEAR: assert(s->reg==0x228 && !prot); break;
  case Y1_DELAY_US: delays[ndelays++]=s->value; break;
  default: assert(0);
  }
  if(!(before&Y1_PWR_RST_B) && (ctl&Y1_PWR_RST_B)) clock_released_reset=!!(ctl&Y1_PWR_CLK_DIS);
  /* Bus protection is held whenever the domain is not fully up. */
  if(s->op<Y1_INFRA_SET && (ctl&(Y1_PWR_ISO|Y1_PWR_CLK_DIS)) && s->op!=Y1_SPM_WRITE) assert(prot==0x11);
  settle();
 }
}
int main(void){
 ctl=Y1_PWR_ISO|Y1_PWR_CLK_DIS; prot=0x11; settle();
 run(y1_conn_domain_on,sizeof(y1_conn_domain_on)/sizeof(y1_conn_domain_on[0]));
 assert(ctl==(Y1_PWR_ON|Y1_PWR_ON_2ND|Y1_PWR_RST_B) && !prot && key==1);
 assert(clock_released_reset && ndelays==2 && delays[0]==1 && delays[1]==3);
 ndelays=0;
 run(y1_conn_domain_off,sizeof(y1_conn_domain_off)/sizeof(y1_conn_domain_off[0]));
 assert(ctl==(Y1_PWR_ISO|Y1_PWR_CLK_DIS) && prot==0x11 && !sta && !sta2 && key==2);
 assert(ndelays==2 && delays[0]==1 && delays[1]==1);
}
''')

    def test_domain_sequence_is_the_b1_transcription(self):
        # out/y1-bluetooth-b1-notes.md §1.1 steps 6a-6j and §1.2 step 1, in order.
        on = [('SPM_WRITE', 0, 0x0b160001), ('SPM_SET', 0x280, 4), ('DELAY_US', 0, 1), ('SPM_SET', 0x280, 8),
              ('DELAY_US', 0, 3), ('SPM_POLL_SET', 0x60c, 2), ('SPM_POLL_SET', 0x610, 2),
              ('SPM_CLEAR', 0x280, 0x10), ('SPM_CLEAR', 0x280, 2), ('SPM_SET', 0x280, 0x10),
              ('SPM_SET', 0x280, 1), ('SPM_CLEAR', 0x280, 0x10), ('INFRA_CLEAR', 0x220, 0x11),
              ('INFRA_POLL_CLEAR', 0x228, 0x11)]
        off = [('SPM_WRITE', 0, 0x0b160001), ('INFRA_SET', 0x220, 0x11), ('INFRA_POLL_SET', 0x228, 0x11),
               ('SPM_SET', 0x280, 0x10), ('SPM_SET', 0x280, 2), ('SPM_CLEAR', 0x280, 1),
               ('SPM_CLEAR', 0x280, 4), ('DELAY_US', 0, 1), ('SPM_CLEAR', 0x280, 8), ('DELAY_US', 0, 1),
               ('SPM_POLL_CLEAR', 0x60c, 2), ('SPM_POLL_CLEAR', 0x610, 2)]
        names = 'SPM_WRITE SPM_SET SPM_CLEAR SPM_POLL_SET SPM_POLL_CLEAR INFRA_SET INFRA_CLEAR ' \
                'INFRA_POLL_SET INFRA_POLL_CLEAR DELAY_US'.split()
        def table(steps):
            return '\n'.join(f' assert(t[{i}].op==Y1_{op} && t[{i}].reg=={reg} && t[{i}].value=={value}u);'
                             for i, (op, reg, value) in enumerate(steps))
        run_c(r'''
#include <assert.h>
''' + POLICY + r'''
int main(void){
 const struct y1_conn_step *t=y1_conn_domain_on;
 assert(sizeof(y1_conn_domain_on)/sizeof(*t)==''' + str(len(on)) + ''');
''' + table(on) + r'''
 t=y1_conn_domain_off;
 assert(sizeof(y1_conn_domain_off)/sizeof(*t)==''' + str(len(off)) + ''');
''' + table(off) + '''
 assert(''' + ' && '.join(f'Y1_{n}=={i}' for i, n in enumerate(names)) + ''');
}
''')

    def test_actual_board_config_takes_exactly_the_y1_key_set(self):
        body = function((Y1 / 'wmt.c').read_text(), 'board_config')
        cases = [(STOCK_CFG, 0), ('# stock\n\n' + '\r\n'.join(reversed(STOCK_CFG.split())) + '\n', 0),
                 (STOCK_CFG + 'co_clock_flag=1\n', -22), (STOCK_CFG.replace('sdio_driving_cfg=0\n', ''), -22),
                 (STOCK_CFG + 'wmt_gps_lna_pin=0\n', -22), (STOCK_CFG.replace('=1', '=0', 1), -22),
                 # The Y2's own file: co-clock, no SDIO driving.
                 ('coex_wmt_ant_mode=1\nco_clock_flag=1\nwmt_gps_lna_pin=0\nwmt_gps_lna_enable=0\n', -22),
                 ('', -22), (STOCK_CFG + '#' * 1024, -22)]
        checks = '\n'.join(f' load({c_string(text)},{len(text.encode())});assert(board_config(&c)=={want});'
                           for text, want in cases)
        run_c(r'''
#define _GNU_SOURCE
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
''' + POLICY + r'''
#define Y2_CONN_FW "mediatek/mt6572/"
#define GFP_KERNEL 0
struct firmware {size_t size; const unsigned char *data;};
struct y2_conn {void *dev;};
static struct firmware blob; static unsigned char buffer[2048]; static unsigned released;
static int y2_conn_request_firmware(void *dev,const char *name,const struct firmware **fw){
 (void)dev; assert(!strcmp(name,"mediatek/mt6572/WMT_SOC.cfg")); *fw=&blob; return 0;
}
static void release_firmware(const struct firmware *fw){assert(fw==&blob);released++;}
static char *kmemdup_nul(const void *p,size_t n,int flags){(void)flags;return strndup(p,n);}
static void kfree(void *p){free(p);}
static char *strim(char *s){
 while(isspace((unsigned char)*s))s++;
 char *e=s+strlen(s);while(e>s && isspace((unsigned char)e[-1]))*--e=0;return s;
}
static void load(const char *text,size_t n){memcpy(buffer,text,n);blob.size=n;blob.data=buffer;}
''' + body + r'''
int main(void){
 struct y2_conn c={0};
''' + checks + r'''
 load("coex_wmt_ant_mode=1\0x",21);assert(board_config(&c)==-22);
 assert(released==''' + str(len(cases) + 1) + r''');
}
''')

    def test_single_part_patch_header_and_actual_download(self):
        entry = next(f for f in INVENTORY['files'] if f['filename'] == 'mt6572_82_patch_e1_0_hdr.bin')
        header = entry['header']
        raw = (header['build_time'] + '\n').encode() + header['platform'].encode() + \
            bytes.fromhex(header['hw_version_hex']) + bytes.fromhex(header['fw_version_hex']) + \
            bytes.fromhex(header['patch_info_hex'])
        self.assertEqual(len(raw), 28)
        address = bytes.fromhex(entry['download_address_wire_hex'])
        body = function((Y1 / 'wmt.c').read_text(), 'patch')
        run_c(r'''
#include <assert.h>
#include <errno.h>
#include <string.h>
''' + POLICY + r'''
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
#define dev_info(...) ((void)0)
#define dev_err(...) ((void)0)
struct firmware {size_t size; const unsigned char *data;};
struct y2_conn {void *dev; unsigned fvr;};
static unsigned char image[''' + str(entry['bytes']) + r''']; static struct firmware blob={sizeof(image),image};
static unsigned char sent[64][1005]; static unsigned sizes[64], count, payload, fail_at=~0u;
static int y2_conn_request_firmware(void *dev,const char *name,const struct firmware **fw){
 (void)dev; assert(!strcmp(name,"x")); *fw=&blob; return 0;
}
static void release_firmware(const struct firmware *fw){assert(fw==&blob);}
static int y2_conn_wmt(struct y2_conn *c,const unsigned char *r,unsigned n,const unsigned char *e,unsigned en){
 (void)c;(void)e;(void)en; assert(count<64 && n<=1005); memcpy(sent[count],r,n); sizes[count]=n;
 if(r[1]==1) payload+=n-5;
 return count++==fail_at?-5:0;
}
''' + body + r'''
int main(void){
 static const unsigned char header[]=''' + c_bytes(raw) + r''', wire[]=''' + c_bytes(address) + r''';
 unsigned char out[4];
 struct y2_conn c={0,0x8a00};
 memcpy(image,header,28); for(unsigned i=28;i<sizeof(image);i++) image[i]=(unsigned char)i;
 assert(!y1_conn_patch_header(image,sizeof(image),0x8a00,out) && !memcmp(out,wire,4));
 /* The launcher compares only the FVR's low byte (byte 23). */
 assert(!y1_conn_patch_header(image,sizeof(image),0x9900,out));
 assert(y1_conn_patch_header(image,sizeof(image),0x8a01,out));
 assert(y1_conn_patch_header(image,28,0x8a00,out) && y1_conn_patch_header(image,65537,0x8a00,out));
 for(unsigned at=16;at<25;at++){
  if(at==22) continue; /* not compared by stock */
  image[at]^=0x40; assert(y1_conn_patch_header(image,sizeof(image),0x8a00,out)); image[at]^=0x40;
 }
 /* The Y2's two-part rule: "ALPS" and 0x20|sequence. */
 memcpy(image+16,"ALPS",4); image[24]=0x21; assert(y1_conn_patch_header(image,sizeof(image),0x8a00,out));
 memcpy(image,header,28); image[24]=0x12; assert(y1_conn_patch_header(image,sizeof(image),0x8a00,out));
 memcpy(image,header,28);
 assert(!patch(&c,"x"));
 unsigned fragments=(sizeof(image)-28+999)/1000;
 assert(count==2+fragments+1 && payload==sizeof(image)-28);
 static const unsigned char first[]={1,8,16,0,1,1,0,1,0x3c,2,9,2,0,0,0,0,0xff,0xff,0xff,0xff};
 assert(sizes[0]==20 && !memcmp(sent[0],first,20));
 assert(sizes[1]==20 && sent[1][8]==0xc4 && sent[1][9]==4 && !memcmp(sent[1]+12,wire,4));
 assert(!memcmp(sent[1],first,8) && !memcmp(sent[1]+10,first+10,2) && !memcmp(sent[1]+16,first+16,4));
 for(unsigned i=0;i<fragments;i++){
  unsigned char *r=sent[2+i]; unsigned n=sizes[2+i]-5;
  assert(r[0]==1 && r[1]==1 && (unsigned)(r[2]|r[3]<<8)==n+1);
  assert(r[4]==(i==fragments-1?3:i?2:1) && (i==fragments-1 || n==1000));
  assert(!memcmp(r+5,image+28+1000*i,n));
 }
 static const unsigned char reset[]={1,7,1,0,4};
 assert(sizes[count-1]==5 && !memcmp(sent[count-1],reset,5));
 /* A wrong header sends nothing; a refused fragment stops the download before RESET. */
 count=payload=0; image[24]=0x22; assert(patch(&c,"x")==-ENOEXEC && !count); image[24]=0x11;
 fail_at=4; assert(patch(&c,"x")==-5 && count==5);
}
''')

    def test_address_record_rule(self):
        default = bytes.fromhex('000046657201')
        good = bytes.fromhex('0a1b2c3d4e5f') + bytes(range(6, 32))
        cases = [(good, 0), (default + good[6:], -1), (bytes([1]) + good[1:], -1),
                 (bytes(6) + good[6:], -1), (good[:31], -1), (good + b'\0', -1),
                 (bytes.fromhex('000046658201') + good[6:], 0)]
        checks = '\n'.join(
            f' {{static const unsigned char r[]={c_bytes(record + bytes(1))},d[]='
            f'{c_bytes(hashlib.sha256(record[:6]).digest())};'
            f'assert(y1_conn_record_check(r,{len(record)},d)=={want});}}'
            for record, want in cases)
        run_c('#include <assert.h>\n' + POLICY + 'int main(void){\n' + checks + '\n}\n')


if __name__ == '__main__':
    unittest.main()
