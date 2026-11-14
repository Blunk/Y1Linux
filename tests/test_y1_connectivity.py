"""Innioasis Y1 Bluetooth B2: the MT6572 connectivity owner's own logic, on the host.

docs/planning/Y1-BLUETOOTH-PLAN.md B2. The Y1's facts live in kernel/y1/connectivity/y1-conn-policy.h
(pure C); the Y1's wmt.c functions are compiled here from their actual text with mocks, as
tests/test_connectivity.py does for the Y2. The C cases need clang (the build sandbox has it).
"""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile
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

    def test_firmware_table_matches_the_inventory_and_has_no_modem(self):
        header = (Y1 / 'conn.h').read_text()
        self.assertIn('#define Y2_CONN_FW "mt6572/"', header)
        rows = re.findall(r'\{Y2_CONN_FW "([^"]+)", (\d+), \{([^}]*)\}\}', header)
        table = {name: (int(size), bytes(int(b, 16) for b in digest.split(',')).hex())
                 for name, size, digest in rows}
        wanted = {f['filename']: (f['bytes'], f['sha256']) for f in INVENTORY['files']
                  if f['filename'] != 'modem_1_2g_n.img'}
        self.assertEqual(table, wanted)
        for f in INVENTORY['files']:
            if f['filename'] in table:
                self.assertTrue(f['proposed_install_path'].startswith('/data/firmware/mt6572/'))

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
        shared = {'btif.c', 'hci.c', 'firmware.c'}
        for name in shared:
            self.assertEqual(added['drivers/bluetooth/y1-conn-' + name], 'kernel/platform/connectivity/' + name)
        self.assertEqual(added['drivers/bluetooth/protocol.h'], 'kernel/platform/connectivity/protocol.h')
        # stp.c stays at the Y1's reviewed copy: the Y2's now carries FM channel code the Y1's conn.h lacks.
        for own in ('core.c', 'wmt.c', 'stp.c'):
            self.assertEqual(added['drivers/bluetooth/y1-conn-' + own], 'kernel/y1/connectivity/' + own)
        self.assertEqual(added['drivers/bluetooth/conn.h'], 'kernel/y1/connectivity/conn.h')
        # No Y2 modem or SPM code in the Y1 driver; the Y2's Wi-Fi subtree is not copied either: the Y1 builds
        # it in place (drivers/y2, mounted in every build) through overlay 0040, only with its own option.
        self.assertFalse(any(re.search(r'md\.c|wifi|domain\.h|spm', s) for s in added.values()))
        makefile = (ROOT / 'kernel/patches-y1/0040-innioasis-y1-wifi-drivers-makefile.patch').read_text()
        added_lines = [l[1:] for l in makefile.splitlines() if l.startswith('+') and not l.startswith('+++')]
        self.assertEqual([l for l in added_lines if not l.startswith('#')],
                         ['obj-$(CONFIG_BT_INNIOASIS_Y1_WIFI)\t+= y2/connectivity/wifi/'])
        kconfig = (ROOT / 'kernel/patches-y1/0036-innioasis-y1-bluetooth-kconfig.patch').read_text()
        self.assertIn('+config BT_INNIOASIS_Y1_WIFI\n+\tbool', kconfig)
        self.assertIn('+\tdepends on BT_INNIOASIS_Y1 && CFG80211=y && RFKILL=y\n', kconfig)

    def test_only_the_y1_tree_names_the_driver_without_a_bring_up_limit(self):
        # The full driver (B6, docs/planning/Y1-BLUETOOTH-PLAN.md): no "innioasis,bring-up" limit.
        compatible = re.search(r'compatible="([^"]+)"', (Y1 / 'core.c').read_text()).group(1)
        self.assertEqual(compatible, 'innioasis,y1-mt6572-connectivity')
        naming = [t.name for t in sorted((ROOT / 'kernel/dts').glob('*y1*')) if compatible in t.read_text()]
        self.assertEqual(naming, ['innioasis-y1.dts'])
        tree = (ROOT / 'kernel/dts/innioasis-y1.dts').read_text()
        node = tree[tree.index('connectivity@18070000 {'):]
        node = node[:node.index('};')]
        self.assertNotIn('innioasis,bring-up', node)
        # No supply of the connectivity core always-on or boot-on, the PA rails included.
        for rail in ('vcn18', 'vcn28', 'vcn33_bt', 'vcn33_wifi'):
            block = tree[tree.index(f'{rail}: ldo_{rail} {{'):]
            block = block[:block.index('};')]
            self.assertNotIn('always-on', block)
            self.assertNotIn('boot-on', block)

    def test_b6_services_with_the_w6_wifi_service(self):
        # The Y2's S41 runs y2-factory and MD calibration: never on the Y1. W6 adds the supplicant and the
        # platform's connectivity monitor, only once the Wi-Fi device exists.
        base = ROOT / 'initramfs/y1-bluetooth/etc/init.d'
        services = (base / 'y1-services').read_text()
        listed = services.split('Y1_SERVICES="')[1].split('"')[0].split()
        self.assertTrue({'S30dbus-daemon', 'S41y2-connectivity', 'S03y1-data'} <= set(listed))
        self.assertLess(listed.index('S03y1-data'), listed.index('S41y2-connectivity'))
        self.assertNotIn('S40bluetoothd', listed)  # y2-radiod starts bluetoothd itself
        script = (base / 'S41y2-connectivity').read_text()
        code = '\n'.join(line for line in script.splitlines() if not line.lstrip().startswith('#'))
        for absent in ('y2-factory', 'y2-calibration', 'y2-radio-activate', '/usr/libexec/y2/connectivity'):
            self.assertNotIn(absent, code)
        for present in ('RECORD=/data/connectivity/bt-record', 'FIRMWARE=/data/firmware/mt6572',
                        '"$1 $3 $5" = "-rw------- 0 32"',
                        '\tmigrate\n\tfirmware\n\tcompose\n\twifi_compose\n\tactivate || exit 1\n\tservices',
                        'y2-platform firmware scan', 'start-stop-daemon -K -p "$WAIT"',
                        'cat "$RECORD" > "$C/factory" || return 1\n\twifi_record\n\techo 1 > "$C/activate"',
                        'activated=1*) wifi_record; return 0;;',
                        '-x /usr/sbin/y2-radiod', 'stop-wifi) y2-radio wifi off-runtime 2>/dev/null || : ;;',
                        'wpa_supplicant -B -u -g /run/wpa_supplicant/global -P /run/y2/wpa.pid',
                        '-p /run/y2/connectivity.pid -x /usr/sbin/y2-platform -- connectivity-daemon',
                        '[ -e "$C/y2-mt6582-wifi" ] && wifi=1',
                        'mount --bind /data/bluetooth/bluez /var/lib/bluetooth',
                        "'ctrl_interface=/run/wpa_supplicant' 'update_config=1' 'country=00'",
                        "[ -e /data/network/enabled ] || printf '0\\n' > /data/network/enabled"):
            self.assertIn(present, script)
        # The supplicant and the monitor start after radiod is asked for, and only with the Wi-Fi device.
        body = re.search(r'^services\(\) \{\n.*?^\}\n', script, re.M | re.S)[0]
        self.assertLess(body.index('[ -n "$wifi" ] || return 0'), body.index('connectivity-daemon'))
        self.assertLess(body.index('if [ -n "$wifi" ]'), body.index('wpa_supplicant -B'))
        listing = (ROOT / 'initramfs/y1-production.list').read_text()
        self.assertIn('/y1-system/etc/init.d/S41y2-connectivity /project/initramfs/y1-bluetooth/etc/init.d/'
                      'S41y2-connectivity 0755', listing)
        pid1 = (ROOT / 'initramfs/y1-system.h').read_text()
        self.assertIn('    /* Bluetooth: hci0 from the owner\'s record, then y2-radiod', pid1)
        self.assertIn('{ "/etc/y2linux/radio-firmware.json", 0644 },\n', pid1)
        self.assertIn('file /y1-system/etc/y2linux/radio-firmware.json /project/initramfs/y1-bluetooth/etc/y2linux/'
                      'radio-firmware.json 0644', listing)

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
 /* Wi-Fi (MediaTek's MT6572 CONN_HIF_BASE and MT_WF_HIF_IRQ_ID), optional in the node; no WMT command. */
 assert(!strcmp(y1_conn_wifi_range.name,"wifi") && y1_conn_wifi_range.start==0x180f0000 &&
        y1_conn_wifi_range.size==0x5c && !strcmp(y1_conn_wifi_irq.name,"wifi") && y1_conn_wifi_irq.spi==123);
 assert(Y1_CONN_WIFI_WMT_FUNCTION==0 && Y1_CONN_WIFI_RECORD_SIZE==512 && Y1_CONN_WIFI_MAC==4);
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
#define Y2_CONN_FW "mt6572/"
#define GFP_KERNEL 0
#define dev_err(...) ((void)0)
#define dev_info(...) ((void)0)
struct firmware {size_t size; const unsigned char *data;};
struct y2_conn {void *dev;};
static struct firmware blob; static unsigned char buffer[2048]; static unsigned released;
static int y2_conn_request_firmware(void *dev,const char *name,const struct firmware **fw){
 (void)dev; assert(!strcmp(name,"mt6572/WMT_SOC.cfg")); *fw=&blob; return 0;
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

    def test_wifi_record_rule(self):
        # A synthetic stand-in for the stock default: the rule compares the record, MAC zeroed, with the
        # pinned hash, so the pin is swapped for this default's (the real pin is checked below).
        default = bytearray(512)
        default[0:2] = b'\x04\x01'
        default[196] = 1
        bt = bytes.fromhex('0a1b2c3d4e5f')
        def record(mac, base=default):
            r = bytearray(base)
            r[4:10] = mac
            return bytes(r)
        changed = bytearray(default)
        changed[12] = 0x30  # another TX power: not the stock default
        cases = [(record(bytes.fromhex('020000aabbcc')), 0), (record(bytes(6)), -1),
                 (record(bytes.fromhex('030000aabbcc')), -1), (record(bt), -1),
                 (record(bytes.fromhex('020000aabbcc'), changed), -1), (record(bytes.fromhex('020000aabbcc'))[:511], -1)]
        pin = c_bytes(hashlib.sha256(bytes(default)).digest())
        policy = (Y1 / 'y1-conn-policy.h').read_text()
        policy = re.sub(r'#define Y1_CONN_WIFI_DEFAULT_SHA256 \{[^}]*\}', '#define Y1_CONN_WIFI_DEFAULT_SHA256 ' + pin,
                        policy)
        checks = '\n'.join(
            f' {{static const unsigned char r[]={c_bytes(rec + bytes(1))},d[]='
            f'{c_bytes(hashlib.sha256(rec[:4] + bytes(6) + rec[10:]).digest())},b[]={c_bytes(bt)};'
            f'assert(y1_conn_wifi_record_check(r,{len(rec)},d,b)=={want});}}'
            for rec, want in cases)
        run_c('#include <assert.h>\n#pragma clang diagnostic ignored "-Wunused-const-variable"\n'
              '#pragma clang diagnostic ignored "-Wunused-function"\n' + policy + 'int main(void){\n' + checks + '\n}\n')

    def test_wifi_default_pin_is_the_inventory_record(self):
        policy = (Y1 / 'y1-conn-policy.h').read_text()
        block = policy[policy.index('#define Y1_CONN_WIFI_DEFAULT_SHA256'):]
        digest = bytes(int(b, 16) for b in re.findall(r'0x([0-9a-f]{2})', block[:block.index('}')]))
        self.assertEqual(digest.hex(), INVENTORY['wifi_default_record']['sha256'])

    def test_wifi_function_uses_its_pa_rail_and_no_wmt_command(self):
        core = (Y1 / 'core.c').read_text().replace('int y2_conn_rail(', 'static int y2_conn_rail(')
        run_c(MODEL + r"""
static bool pmic_allowed(unsigned reg,unsigned mask){return (reg==0x416 && mask==BIT(5)) || (reg==0x418 && mask==BIT(14));}
#define Y2_CONN_BT 0
#define Y2_CONN_WIFI 3
static unsigned switched[8],nswitched;
static int y2_wmt_function(struct y2_conn *c,unsigned f,bool e){(void)c;switched[nswitched++]=f<<1|e;return 0;}
""" + '\n'.join(function(core, name) for name in ('y2_conn_rail', 'function_rail', 'function_switch')) + r"""
int main(void){
 struct y2_conn c;
 seed(&c,""" + str(BASELINE['ctl']) + r""",0x6572,false,Y1_MODE_FULL,true);
 assert(function_rail(Y2_CONN_BT)==2 && function_rail(Y2_CONN_WIFI)==3);
 /* Wi-Fi on: VCN33_WIFI enabled and under hardware control (MediaTek's HifAhbProbe), no WMT command. */
 assert(!y2_conn_rail(&c,function_rail(Y2_CONN_WIFI),true) && !function_switch(&c,Y2_CONN_WIFI,true));
 assert((pmic[0x418]&BIT(12)) && (pmic[0x418]&BIT(14)) && c.rail_on[3] && !c.rail_on[2] && !nswitched);
 assert(!(pmic[0x416]&BIT(7)) && !(pmic[0x416]&BIT(5)));
 /* Bluetooth still sends its function command. */
 assert(!function_switch(&c,Y2_CONN_BT,true) && nswitched==1 && switched[0]==1);
 /* Wi-Fi off: the rail and its hardware mode cleared, again no command. */
 assert(!function_switch(&c,Y2_CONN_WIFI,false) && !y2_conn_rail(&c,function_rail(Y2_CONN_WIFI),false));
 assert(!(pmic[0x418]&(BIT(12)|BIT(14))) && !c.rail_on[3] && nswitched==1);
}
""")


# The B4 baseline (evidence-private/2026-11-06-y1-bt-b4/baseline.txt): CONN off, MD1 on from LK.
BASELINE = {'ctl': 0x1112, 'status': 0x3ffd, 'sleep': 0x080000f9, 'protect': 0, 'pmic_512': 0, 'pmic_41c': 0,
            'pmic_416': 0x001c, 'pmic_418': 0}
POWER_PATH = ('y2_conn_rail', 'step', 'domain_on', 'domain_off', 'sleep_wait', 'rails_off', 'power_off',
              'normalise', 'vcn18_on', 'vcn28_on', 'spm_clock', 'chip_id', 'stage', 'stages')


def power_path(extra=()):
    core = (Y1 / 'core.c').read_text().replace('int y2_conn_rail(', 'static int y2_conn_rail(')
    return '\n'.join(function(core, name) for name in POWER_PATH + tuple(extra))


# A register model of SPM, INFRA and the MT6323 seeded with the baseline; the driver's power path runs on it
# unchanged. Rails: 0 VCN18 (0x512 bit 14), 1 VCN28 (0x41c bit 12), 2 VCN33_BT (0x416 bit 7), 3 VCN33_WIFI
# (0x418 bit 12). The MCU window may be read only with the domain fully up.
MODEL = r"""
/* Each case uses a part of the model's helpers. */
#pragma clang diagnostic ignored "-Wunused-function"
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>
""" + POLICY + r"""
#define BIT(n) (1u<<(n))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define WRITE_ONCE(x,v) ((x)=(v))
#define READ_ONCE(x) (x)
static unsigned errors,warnings;
static void dev_info(void *d,const char *f,...){(void)d;(void)f;}
static void dev_err(void *d,const char *f,...){(void)d;(void)f;errors++;}
static void dev_warn(void *d,const char *f,...){(void)d;(void)f;warnings++;}
struct regmap {int which;}; struct clk {int unused;};
struct regulator {unsigned reg,bit;}; struct regulator_bulk_data {const char *supply; struct regulator *consumer;};
struct clk_bulk_data {const char *id; struct clk *clk;};
struct y2_conn {
 void *dev; struct regmap *pmic,*spm,*infracfg; struct regulator_bulk_data rails[4];
 struct clk_bulk_data clocks[3]; bool rail_on[4],clock_on,powered,dma_active,abort,calibrated;
 unsigned char *emi; char *mcu; unsigned chip,hvr,fvr,mode,calibration_size,calibration_status;
 int failure,lifecycle; unsigned response_size; unsigned char response_data[256];
};
static struct regmap PMIC={0},SPM={1},INFRA={2};
static unsigned ctl,sta,sleep_reg,protect,pmic[0x600],mcu_reads,mcu_value,events[64],nevents;
static bool sleep_stuck,asleep,ack_missing,ack_missing_when_asleep;
/* B5 run: once the core slept, 0x228 never acknowledged the protection (0xf000; B4: 0xf011). */
static bool ack_withheld(void){return ack_missing || (ack_missing_when_asleep && !(sleep_reg&Y1_SPM_CONN_AWAKE));}
enum {E_VCN18_LP,E_VCN18_ON,E_VCN28_HW,E_VCN28_ON,E_EMI,E_CLOCK,E_DOMAIN,E_CHIP,E_PROTECT,E_VCN18_OFF,
      E_VCN28_OFF,E_BTIF,E_PA_ON,E_CALIBRATE,E_PA_OFF,E_SLEEP,E_BTIF_STOP};
static void event(unsigned e){assert(nevents<64);events[nevents++]=e;}
static bool domain_up(void){return (ctl&0xf)==0xd && !(ctl&0x12) && !protect;}
static void model(void){
 bool on=(ctl&(Y1_PWR_ON|Y1_PWR_ON_2ND))==(Y1_PWR_ON|Y1_PWR_ON_2ND);
 sta=(sta&~2u)|(on?2u:0);
 if(on && (ctl&1) && !asleep) sleep_reg|=sleep_stuck?Y1_SPM_CONN_AWAKE:0;
 if(on && (ctl&1) && asleep) sleep_reg&=~Y1_SPM_CONN_AWAKE;
 if(!on) sleep_reg&=~Y1_SPM_CONN_AWAKE;
}
static int regmap_read(struct regmap *m,unsigned reg,unsigned *v){
 if(m==&SPM){assert(reg==0x280||reg==0x60c||reg==0x610||reg==0x3b4);
  *v=reg==0x280?ctl:reg==0x3b4?sleep_reg:sta;}
 else if(m==&INFRA){assert(reg==0x220||reg==0x228);*v=reg==0x220?protect:(0xf000|(ack_withheld()?0:protect));}
 else {assert(reg==0x512||reg==0x41c);*v=pmic[reg];}
 return 0;
}
static int regmap_write(struct regmap *m,unsigned reg,unsigned v){
 assert(m==&SPM && reg==0 && v==0x0b160001);return 0;
}
static bool pmic_allowed(unsigned reg,unsigned mask);
static int regmap_update_bits(struct regmap *m,unsigned reg,unsigned mask,unsigned v){
 if(m==&SPM){assert(reg==0x280);ctl=(ctl&~mask)|v;model();return 0;}
 if(m==&INFRA){assert(reg==0x220 && mask==0x11);protect=(protect&~mask)|v;if(v)event(E_PROTECT);return 0;}
 assert(m==&PMIC && pmic_allowed(reg,mask));
 pmic[reg]=(pmic[reg]&~mask)|v;
 if(reg==0x512) event(E_VCN18_LP); else if(reg==0x41c && v) event(E_VCN28_HW);
 return 0;
}
static int regmap_set_bits(struct regmap *m,unsigned reg,unsigned bits){return regmap_update_bits(m,reg,bits,bits);}
static int regmap_clear_bits(struct regmap *m,unsigned reg,unsigned bits){return regmap_update_bits(m,reg,bits,0);}
#define regmap_read_poll_timeout(m,reg,val,cond,sleep_us,timeout_us) ({ int r_=-ETIMEDOUT; \
 for(unsigned i_=0;i_<5;i_++){ regmap_read(m,reg,&(val)); if(cond){r_=0;break;} } r_; })
static unsigned mcu_read(void){assert(domain_up());mcu_reads++;event(E_CHIP);return mcu_value;}
#define readl_poll_timeout(addr,val,cond,sleep_us,timeout_us) ({ int r_=-ETIMEDOUT; (void)(addr); \
 for(unsigned i_=0;i_<10;i_++){ (val)=mcu_read(); if(cond){r_=0;break;} } r_; })
static inline void udelay(unsigned us){(void)us;}
static inline void msleep(unsigned ms){(void)ms;}
static inline void usleep_range(unsigned a,unsigned b){(void)a;(void)b;}
static void memset_io(void *p,int v,unsigned n){assert(n==Y1_CONN_EMI_SIZE && !v && p && !(ctl&1));event(E_EMI);}
static bool clock_enabled;
static int clk_prepare_enable(struct clk *c){(void)c;assert(!clock_enabled);clock_enabled=true;event(E_CLOCK);return 0;}
static int regulator_enable(struct regulator *r){
 pmic[r->reg]|=r->bit;
 event(r->reg==0x512?E_VCN18_ON:r->reg==0x41c?E_VCN28_ON:E_PA_ON);return 0;
}
static int regulator_disable(struct regulator *r){
 pmic[r->reg]&=~r->bit;
 event(r->reg==0x512?E_VCN18_OFF:r->reg==0x41c?E_VCN28_OFF:E_PA_OFF);return 0;
}
static struct regulator vcn18={0x512,BIT(14)},vcn28={0x41c,BIT(12)},vcn33_bt={0x416,BIT(7)},vcn33_wifi={0x418,BIT(12)};
static unsigned char emi[1];
static void seed(struct y2_conn *c,unsigned start_ctl,unsigned chip,bool stuck,unsigned mode,bool pa){
 ctl=start_ctl;sta=""" + str(BASELINE['status']) + r""";sleep_reg=""" + str(BASELINE['sleep']) + r""";
 protect=""" + str(BASELINE['protect']) + r""";pmic[0x512]=0;pmic[0x41c]=0;
 pmic[0x416]=""" + str(BASELINE['pmic_416']) + r""";pmic[0x418]=0;
 if(start_ctl&1){sta|=2;protect=0;}
 mcu_value=chip;sleep_stuck=stuck;asleep=false;errors=warnings=mcu_reads=nevents=0;clock_enabled=false;
 memset(c,0,sizeof(*c));c->pmic=&PMIC;c->spm=&SPM;c->infracfg=&INFRA;c->emi=emi;c->mcu=(char *)emi;c->mode=mode;
 c->rails[0].consumer=&vcn18;c->rails[1].consumer=&vcn28;
 if(pa){c->rails[2].consumer=&vcn33_bt;c->rails[3].consumer=&vcn33_wifi;}
}
static void back_at_baseline(struct y2_conn *c){
 /* The baseline control value and status; the bus protection now set, as stock leaves it. */
 assert(ctl==""" + str(BASELINE['ctl']) + r""" && sta==""" + str(BASELINE['status']) + r""" && protect==0x11);
 assert(pmic[0x512]==0 && pmic[0x41c]==0 && pmic[0x418]==0 && pmic[0x416]==""" + str(BASELINE['pmic_416']) + r""");
 assert(!c->powered && !c->rail_on[0] && !c->rail_on[1] && !c->rail_on[2] && !c->rail_on[3]);
 assert(!c->dma_active && c->clock_on);
}
static unsigned position(unsigned e){for(unsigned i=0;i<nevents;i++)if(events[i]==e)return i;assert(0);return 0;}
static unsigned count(unsigned e){unsigned n=0;for(unsigned i=0;i<nevents;i++)n+=events[i]==e;return n;}
"""


class Y1ChipIdBringUp(unittest.TestCase):
    def test_stage_lists_and_modes(self):
        run_c(r"""
#include <assert.h>
#include <string.h>
""" + POLICY + r"""
int main(void){
 static const unsigned char chip[]={Y1_STAGE_NORMALISE,Y1_STAGE_VCN18,Y1_STAGE_VCN28,Y1_STAGE_EMI_CLEAR,
  Y1_STAGE_SPM_CLOCK,Y1_STAGE_DOMAIN_ON,Y1_STAGE_CHIP_ID,Y1_STAGE_OFF};
 assert(sizeof(chip)==sizeof(y1_conn_chip_id_stages) && !memcmp(chip,y1_conn_chip_id_stages,sizeof(chip)));
 /* The full driver: the same power-on, then BTIF and WMT, never OFF on success. */
 assert(sizeof(y1_conn_full_stages)==sizeof(chip)+1 && !memcmp(y1_conn_full_stages,chip,7));
 assert(y1_conn_full_stages[7]==Y1_STAGE_BTIF && y1_conn_full_stages[8]==Y1_STAGE_WMT);
 for(unsigned i=0;i<sizeof(chip);i++) assert(chip[i]!=Y1_STAGE_BTIF && chip[i]!=Y1_STAGE_WMT);
 assert(sizeof(y1_conn_stage_names)/sizeof(*y1_conn_stage_names)==Y1_STAGE_OFF+1);
 assert(!strcmp(y1_conn_stage_names[Y1_STAGE_CHIP_ID],"chip-id") && !strcmp(y1_conn_stage_names[Y1_STAGE_OFF],"off"));
 assert(y1_conn_mode("chip-id")==Y1_MODE_CHIP_ID && y1_conn_mode("wmt")==Y1_MODE_WMT);
 assert(y1_conn_mode("")<0 && y1_conn_mode("wm")<0 && y1_conn_mode("wmt ")<0 && y1_conn_mode("full")<0);
}
""")

    def test_actual_power_path_from_the_b4_baseline(self):
        run_c(MODEL + r"""
/* Only the two bits the chip-id bring-up owns; never a PA rail or the PMIC init subset. */
static bool pmic_allowed(unsigned reg,unsigned mask){
 return (reg==0x512 && mask==Y1_PMIC_VCN18_LP) || (reg==0x41c && mask==Y1_PMIC_VCN28_HW_MODE);
}
static int y2_btif_stop(struct y2_conn *c){assert(!c->dma_active);return 0;}
static void y2_stp_reset(struct y2_conn *c){(void)c;}
static int y2_btif_abort(struct y2_conn *c){(void)c;assert(0);return 0;}
static int y2_btif_start(struct y2_conn *c){(void)c;assert(0);return 0;}
static int wmt(struct y2_conn *c){(void)c;assert(0);return 0;}
""" + power_path() + r"""
static int run(unsigned start,unsigned chip,bool stuck,struct y2_conn *c){
 seed(c,start,chip,stuck,Y1_MODE_CHIP_ID,false);
 return stages(c,y1_conn_chip_id_stages,ARRAY_SIZE(y1_conn_chip_id_stages));
}
int main(void){
 struct y2_conn c;
 assert(!run(""" + str(BASELINE['ctl']) + r""",0x6572,false,&c) && c.chip==0x6572 && mcu_reads==1);
 assert(!errors && !warnings);
 back_at_baseline(&c);
 /* Stock order: VCN18 (low power off, on), VCN28 (hardware mode, on), memory, SPM clock, domain, ID;
  * then protection, VCN18 off, VCN28 off. */
 assert(position(E_VCN18_LP)<position(E_VCN18_ON) && position(E_VCN18_ON)<position(E_VCN28_HW));
 assert(position(E_VCN28_HW)<position(E_VCN28_ON) && position(E_VCN28_ON)<position(E_EMI));
 assert(position(E_EMI)<position(E_CLOCK) && position(E_CLOCK)<position(E_CHIP));
 assert(position(E_CHIP)<position(E_PROTECT) && position(E_PROTECT)<position(E_VCN18_OFF));
 assert(position(E_VCN18_OFF)<position(E_VCN28_OFF) && !count(E_PA_ON));
 /* No chip ID: still powered off cleanly; the timeout is the result. */
 assert(run(""" + str(BASELINE['ctl']) + r""",0,false,&c)==-ETIMEDOUT && mcu_reads==10 && c.chip==0);
 back_at_baseline(&c);
 /* A core that never sleeps: a warning, then isolation anyway. */
 assert(!run(""" + str(BASELINE['ctl']) + r""",0x6572,true,&c) && warnings==1);
 back_at_baseline(&c);
 /* CONN found on: taken down first (normalise), then the bring-up as usual. */
 assert(!run(0x110d,0x6572,false,&c) && warnings==1 && mcu_reads==1);
 back_at_baseline(&c);
 /* The protection unacknowledged with the core asleep: logged, isolated, success (stock's WARN path). */
 ack_missing_when_asleep=true;
 assert(!run(""" + str(BASELINE['ctl']) + r""",0x6572,false,&c) && !errors);
 back_at_baseline(&c);
 /* Unacknowledged and the core never slept: isolated and switched off all the same, but an error. */
 ack_missing_when_asleep=false;ack_missing=true;
 assert(run(""" + str(BASELINE['ctl']) + r""",0x6572,true,&c)==-ETIMEDOUT && warnings==1 && errors);
 back_at_baseline(&c);
 ack_missing=false;
}
""")


class Y1WmtBringUp(unittest.TestCase):
    """B5: one attempt = the PMIC subset, the full stages (BTIF, WMT with the calibration), WMT sleep, OFF."""

    def harness(self, main):
        wmt_source = (Y1 / 'wmt.c').read_text()
        run_c(MODEL + r"""
/* The chip-id bits plus the PA rails' hardware-mode bits (0x416 bit 5, 0x418 bit 14), as the Y2. */
static bool pmic_allowed(unsigned reg,unsigned mask){
 return (reg==0x512 && mask==Y1_PMIC_VCN18_LP) || (reg==0x41c && mask==Y1_PMIC_VCN28_HW_MODE) ||
  (reg==0x416 && mask==BIT(5)) || (reg==0x418 && mask==BIT(14));
}
static unsigned cal_size=600,fail_opcode,abort_at_opcode,pmic_prepared,wmt_commands[32],nwmt;
static int wmt_error;
static int y2_conn_rail(struct y2_conn *c,unsigned rail,bool enable);
static int wmt(struct y2_conn *c);
static void mutex_lock(int *m){assert(!*m);*m=1;}
static void mutex_unlock(int *m){assert(*m);*m=0;}
static int pmic_prepare_logged(struct y2_conn *c){(void)c;pmic_prepared++;return 0;}
static void log_off_state(struct y2_conn *c,const char *mode){(void)c;(void)mode;}
static int y2_btif_start(struct y2_conn *c){
 assert(domain_up() && mcu_reads && !c->dma_active);c->dma_active=true;event(E_BTIF);return 0;
}
static int y2_btif_stop(struct y2_conn *c){if(c->dma_active)event(E_BTIF_STOP);c->dma_active=false;return 0;}
static void y2_stp_reset(struct y2_conn *c){(void)c;}
static int y2_btif_abort(struct y2_conn *c){(void)c;assert(0);return 0;}
/* The WMT model: every command needs BTIF up and the domain on; the RF calibration needs both PA rails
 * on (and only then); the reply length of the calibration is cal_size. */
static int y2_conn_wmt(struct y2_conn *c,const unsigned char *r,unsigned n,const unsigned char *e,unsigned en){
 (void)n;(void)e;(void)en;
 assert(c->dma_active && domain_up() && nwmt<32);wmt_commands[nwmt++]=r[1];
 if(r[1]==abort_at_opcode) c->abort=true;
 if(c->abort) return -ECANCELED;
 if(r[1]==fail_opcode) return wmt_error;
 bool pa=(pmic[0x416]&BIT(7)) && (pmic[0x418]&BIT(12));
 if(r[1]==0x14){assert(pa && c->rail_on[2] && c->rail_on[3]);event(E_CALIBRATE);
  c->response_size=cal_size;c->response_data[0]=2;c->response_data[1]=0x14;c->response_data[4]=0;c->response_data[5]=1;}
 else {assert(!pa && !(pmic[0x416]&BIT(7)) && !(pmic[0x418]&BIT(12)));c->response_size=en;}
 if(r[1]==3){event(E_SLEEP);asleep=true;model();}
 return 0;
}
""" + function(wmt_source, 'calibrate') + r"""
static int y1_wmt_boot(struct y2_conn *c){
 static const unsigned char version[]={1,8,16,0,2},patch[]={1,1,1,0,1},coex[]={1,0x10,2,0,1,1};
 int ret=y2_conn_wmt(c,version,5,0,16);
 if(!ret){c->chip=0x6572;c->hvr=0x8a00;c->fvr=0x8a00;ret=y2_conn_wmt(c,patch,5,0,5);}
 if(!ret) ret=calibrate(c);
 if(!ret) ret=y2_conn_wmt(c,coex,6,0,5);
 return ret;
}
static int y1_wmt_sleep(struct y2_conn *c){static const unsigned char s[]={1,3,1,0,1};return y2_conn_wmt(c,s,5,0,6);}
""" + power_path(('wmt', 'bring_up_wmt')) + r"""
static bool attempt_awake; /* the core stays awake until a WMT sleep is accepted */
static int attempt(struct y2_conn *c){
 seed(c,""" + str(BASELINE['ctl']) + r""",0x6572,attempt_awake,Y1_MODE_WMT,true);nwmt=0;
 return bring_up_wmt(c);
}
""" + main)

    def test_order_pa_rails_only_for_the_calibration_and_sleep_before_off(self):
        self.harness(r"""
int main(void){
 struct y2_conn c;
 assert(!attempt(&c) && pmic_prepared==1 && !errors && !warnings);
 back_at_baseline(&c);
 assert(c.calibrated && c.calibration_size==600 && c.calibration_status==0x0001);
 /* Power-on as B4, then BTIF, the WMT boot, the calibration inside its PA window, sleep, off. */
 assert(position(E_CLOCK)<position(E_CHIP) && position(E_CHIP)<position(E_BTIF));
 assert(position(E_BTIF)<position(E_PA_ON) && position(E_PA_ON)<position(E_CALIBRATE));
 assert(count(E_PA_ON)==2 && count(E_PA_OFF)==2 && count(E_CALIBRATE)==1);
 for(unsigned i=0;i<nevents;i++) if(events[i]==E_PA_OFF) assert(i>position(E_CALIBRATE) && i<position(E_SLEEP));
 assert(position(E_SLEEP)<position(E_BTIF_STOP) && position(E_BTIF_STOP)<position(E_PROTECT));
 assert(position(E_PROTECT)<position(E_VCN18_OFF) && position(E_VCN18_OFF)<position(E_VCN28_OFF));
 static const unsigned opcodes[]={8,1,0x14,0x10,3};
 assert(nwmt==5 && !memcmp(wmt_commands,opcodes,sizeof(opcodes)));
 /* Any calibration length is taken; a retry runs again from the start. */
 cal_size=6;assert(!attempt(&c) && c.calibration_size==6 && pmic_prepared==2);
 back_at_baseline(&c);
 /* The B5 run: after the accepted sleep 0x228 stays 0xf000; the attempt still succeeds. */
 ack_missing_when_asleep=true;cal_size=630;
 assert(!attempt(&c) && !errors && !warnings && c.calibration_size==630);
 assert(position(E_SLEEP)<position(E_PROTECT));
 back_at_baseline(&c);
 /* A sleep refused: the 10 s wait expires; an unacknowledged protection is then an error, still off. */
 fail_opcode=3;wmt_error=-EPROTO;ack_missing=true;attempt_awake=true;
 assert(attempt(&c)==-ETIMEDOUT && warnings==1 && errors);
 back_at_baseline(&c);
}
""")

    def test_a_failed_step_or_an_abort_powers_off_and_stays_retryable(self):
        self.harness(r"""
int main(void){
 struct y2_conn c;
 /* A refused patch: power-off right away, no PA rail, no sleep command. */
 fail_opcode=1;wmt_error=-EPROTO;
 assert(attempt(&c)==-EPROTO && !count(E_PA_ON) && !count(E_SLEEP) && errors);
 back_at_baseline(&c);
 /* A refused calibration: both PA rails off again, then the power-off. */
 fail_opcode=0x14;
 assert(attempt(&c)==-EPROTO && count(E_PA_ON)==2 && count(E_PA_OFF)==2);
 back_at_baseline(&c);
 /* The abort during the calibration: rails off, no further command, power-off. */
 fail_opcode=0;abort_at_opcode=0x14;
 assert(attempt(&c)==-ECANCELED && count(E_PA_OFF)==2 && !count(E_SLEEP));
 assert(wmt_commands[nwmt-1]==0x14);
 back_at_baseline(&c);
 /* An abort already pending stops before the first stage; a later attempt clears it. */
 abort_at_opcode=0;seed(&c,""" + str(BASELINE['ctl']) + r""",0x6572,false,Y1_MODE_WMT,true);
 c.abort=true;assert(stages(&c,y1_conn_full_stages,ARRAY_SIZE(y1_conn_full_stages))==-ECANCELED && !nevents);
 assert(!attempt(&c) && !c.abort);
 back_at_baseline(&c);
}
""")

    def test_any_calibration_length_but_never_a_bad_status(self):
        run_c('#include <assert.h>\n' + POLICY + r"""
static unsigned char e[2048];
static void make(unsigned n,unsigned char status,unsigned char op){
 e[0]=2;e[1]=0x14;e[2]=(n-4)&0xff;e[3]=(n-4)>>8;e[4]=status;e[5]=op;
}
int main(void){
 const unsigned sizes[]={6,64,256,257,630,1000,2048};
 for(unsigned i=0;i<7;i++){make(sizes[i],0,1);assert(y1_conn_rf_result(e,sizes[i]));}
 make(630,1,1);assert(!y1_conn_rf_result(e,630));
 make(630,0,2);assert(!y1_conn_rf_result(e,630));
 make(630,0,1);assert(!y1_conn_rf_result(e,629));
 e[1]=0x10;assert(!y1_conn_rf_result(e,630));
 make(5,0,1);assert(!y1_conn_rf_result(e,5));
}
""")

    def test_btif_registers_only_after_its_clocks(self):
        source = (ROOT / 'kernel/platform/connectivity/btif.c').read_text()
        defines = '\n'.join(line for line in source.splitlines() if line.startswith('#define '))
        body = defines + '\n' + function(source, 'init_dma') + function(
            source.replace('int y2_btif_start(', 'static int y2_btif_start('), 'y2_btif_start')
        run_c(r"""
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#define Y2_CONN_DMA_SIZE 8192
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define WRITE_ONCE(x,v) ((x)=(v))
#define lower_32_bits(x) ((unsigned)(x))
#define __iomem
typedef unsigned long dma_addr_t;
struct clk_bulk_data {const char *id; void *clk;};
struct y2_conn {struct clk_bulk_data clocks[3]; bool dma_active,transport_on; unsigned txptr,rxptr;
 unsigned char txbuf[8192],rxbuf[8192]; dma_addr_t txaddr,rxaddr; char *btif,*txdma,*rxdma; int irq[4];};
static bool clocks_on; static unsigned writes, irqs;
static int clk_bulk_prepare_enable(int n,struct clk_bulk_data *c){
 assert(n==2 && !strcmp(c[0].id,"btif") && !strcmp(c[1].id,"dma"));clocks_on=true;return 0;
}
static void writel(unsigned v,void *a){(void)v;(void)a;assert(clocks_on);writes++;}
static void enable_irq(int irq){(void)irq;assert(clocks_on && writes);irqs++;}
""" + body + r"""
int main(void){
 static struct y2_conn c={.clocks={{"spm",0},{"btif",0},{"dma",0}}};
 static char window[3][256]; c.btif=window[0];c.txdma=window[1];c.rxdma=window[2];
 assert(!y2_btif_start(&c) && c.dma_active && c.transport_on && writes>20 && irqs==4);
}
""")



class Y1HciB6(unittest.TestCase):
    """B6: the full driver, hci0 through the shared hci.c with the Y1's 32-byte record."""

    def test_actual_setup_injects_the_record_in_stock_order(self):
        # Synthetic record (never the owner's): an address, then the radio/sleep/TX-power bytes.
        record = bytes.fromhex('0a1b2c3d4e5f') + bytes(range(6, 32))
        default = bytes.fromhex('000046657201')
        other = bytes.fromhex('5e1122334455')
        digests = {a: hashlib.sha256(a).digest() for a in (record[:6], default, other)}
        table = '\n'.join(f' {{{c_bytes(a)},{c_bytes(d)}}},' for a, d in digests.items())
        hci = (ROOT / 'kernel/platform/connectivity/hci.c').read_text()
        body = '\n'.join(function(hci, n) for n in ('controller_quirks', 'command', 'setup'))
        quirk_chip = re.search(r'^#define Y2_CONN_EXT_FEATURES_QUIRK_CHIP .*$', (Y1 / 'conn.h').read_text(), re.M)[0]
        run_c(r"""
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
""" + POLICY + r"""
#define Y2_CONN_DEFAULT_ADDRESS_SHA256 Y1_CONN_DEFAULT_ADDRESS_SHA256
""" + quirk_chip + r"""
typedef unsigned short u16;
#define HCI_OP_READ_BD_ADDR 0x1009
#define HCI_OP_RESET 0x0c03
#define HCI_CMD_TIMEOUT 2000
#define HCI_QUIRK_BROKEN_LOCAL_EXT_FEATURES_PAGE_2 1
#define IS_ERR(p) ((unsigned long)(p)>=(unsigned long)-4095)
#define PTR_ERR(p) ((long)(p))
static unsigned quirks;
#define hci_set_quirk(h,q) ((void)(h),quirks|=(q))
#define dev_info(...) ((void)0)
#define memzero_explicit(p,n) memset(p,0,n)
struct sk_buff {unsigned len; unsigned char data[16];};
struct hci_dev {unsigned max_page; void *drv;};
struct y2_conn {struct hci_dev *hdev; unsigned chip,hvr,fvr; bool bt_identity_set,bt_controller_address;
 unsigned char bt_address[6],bt_factory[Y1_CONN_RECORD_SIZE]; void *dev;};
static const struct {unsigned char a[6],d[32];} digests[]={
""" + table + r"""
};
static void sha256(const unsigned char *p,unsigned n,unsigned char *out){
 assert(n==6);for(unsigned i=0;i<sizeof(digests)/sizeof(*digests);i++)
  if(!memcmp(p,digests[i].a,6)){memcpy(out,digests[i].d,32);return;}
 assert(0);
}
static bool is_valid_ether_addr(const unsigned char *a){
 unsigned any=0;for(int i=0;i<6;i++)any|=a[i];return any && !(a[0]&1);
}
static void *hci_get_drvdata(struct hci_dev *h){return h->drv;}
static unsigned char controller[6]; static unsigned ops[16],lens[16],nops; static unsigned char args[16][8];
static void kfree_skb(struct sk_buff *s){free(s);}
static struct sk_buff *__hci_cmd_sync(struct hci_dev *h,unsigned op,unsigned n,const void *data,unsigned t){
 (void)h;(void)t;struct sk_buff *s=calloc(1,sizeof(*s));assert(nops<16 && n<=8);
 ops[nops]=op;lens[nops]=n;if(n)memcpy(args[nops],data,n);nops++;
 if(op==HCI_OP_READ_BD_ADDR){s->len=7;for(int i=0;i<6;i++)s->data[6-i]=controller[i];}
 else s->len=1;
 return s;
}
""" + body + r"""
static void run(const unsigned char *ctl,struct y2_conn *c){
 static const unsigned char record[]=""" + c_bytes(record) + r""";
 static struct hci_dev h; memset(c,0,sizeof(*c)); h.drv=c; h.max_page=2; c->hdev=&h; c->chip=0x6572;
 c->hvr=0x8a01; c->fvr=0x8a00; quirks=0;
 memcpy(c->bt_factory,record,32); memcpy(controller,ctl,6); nops=0;
 assert(!setup(&h));
}
int main(void){
 struct y2_conn c;
 static const unsigned char rec[]=""" + c_bytes(record) + r""", def[]=""" + c_bytes(default) + r""",
  oth[]=""" + c_bytes(other) + r""";
 /* The controller reports the generic MT6572 default: the record's address is used. */
 run(def,&c);
 static const unsigned expect[]={HCI_OP_READ_BD_ADDR,0xfc1a,0xfc79,0xfc93,0xfc7a,HCI_OP_RESET};
 static const unsigned sizes[]={0,6,6,3,7,0};
 assert(nops==6 && !memcmp(ops,expect,sizeof(expect)) && !memcmp(lens,sizes,sizeof(sizes)));
 for(int i=0;i<6;i++) assert(args[1][i]==rec[5-i]); /* address reversed on the wire */
 assert(!memcmp(args[2],rec+12,6) && !memcmp(args[3],rec+27,3) && !memcmp(args[4],rec+18,7));
 assert(!c.bt_controller_address && !memcmp(c.bt_address,rec,6));
 /* B6 run 1: this HVR 8a01/FVR 8a00 firmware rejects extended-features page 2 (status 0x30, -38). */
 assert(quirks==HCI_QUIRK_BROKEN_LOCAL_EXT_FEATURES_PAGE_2 && c.hdev->max_page==1);
 /* A valid, non-default controller identity takes precedence (the Y2's shared rule); settings stay the record's. */
 run(oth,&c);
 assert(c.bt_controller_address && !memcmp(c.bt_address,oth,6));
 for(int i=0;i<6;i++) assert(args[1][i]==oth[5-i]);
 assert(!memcmp(args[2],rec+12,6));
 /* Identity is chosen once per boot: a second setup reads nothing new. */
 nops=0; assert(!setup(c.hdev) && ops[0]==0xfc1a && nops==5);
}
""")

    def test_the_actual_factory_rule_rejects_default_and_bad_records(self):
        cases = [(bytes.fromhex('000046657201') + bytes(26), -1), (bytes.fromhex('02aabbccddee') + bytes(26), 0),
                 (bytes(32), -1), (bytes.fromhex('03aabbccddee') + bytes(26), -1)]
        checks = '\n'.join(f' {{static const unsigned char r[]={c_bytes(r)},d[]={c_bytes(hashlib.sha256(r[:6]).digest())};'
                           f'assert(y1_conn_record_check(r,32,d)=={w});assert(y1_conn_record_check(r,31,d)==-1);}}'
                           for r, w in cases)
        core = (Y1 / 'core.c').read_text()
        self.assertIn('if (offset || size!=Y1_CONN_RECORD_SIZE) return -EINVAL;', core)
        self.assertIn('y1_conn_record_check((const u8 *)data, size, digest)', core)
        run_c('#include <assert.h>\n' + POLICY + 'int main(void){\n' + checks + '\n}\n')

    def test_actual_open_close_sequence(self):
        core = (Y1 / 'core.c').read_text().replace('int y2_conn_get(', 'static int y2_conn_get(') \
            .replace('int y2_conn_put(', 'static int y2_conn_put(')
        hci = (ROOT / 'kernel/platform/connectivity/hci.c').read_text()
        body = '\n'.join(function(core, n) for n in ('sleep_then_off', 'runtime_resume', 'runtime_suspend',
                                                     'function_rail', 'function_switch', 'y2_conn_get', 'y2_conn_put'))
        body += '\n'.join(function(hci, n) for n in ('open', 'flush', 'close'))
        run_c(r"""
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#define BIT(n) (1u<<(n))
#define Y2_CONN_BT 0
#define Y2_CONN_WIFI 3
#define Y1_CONN_WIFI_WMT_FUNCTION 0
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define dev_warn(...) ((void)0)
struct device {int unused;};
struct y2_conn {struct device *dev; int lifecycle,stp_lock,hci_work,hci_tx; void *hci_rx;
 bool activated,recovering,removing,powered,dma_active,full_stp,hci_open,hci_paused;
 unsigned functions,wanted; int failure;};
struct hci_dev {struct y2_conn *c;};
static struct y2_conn conn; static struct device dev;
enum {POWER_ON,PA_ON,FUNC_ON,FUNC_OFF,PA_OFF,SLEEP,POWER_OFF};
static unsigned log_[32],n;
static void rec(unsigned e){assert(n<32);log_[n++]=e;}
static void mutex_lock(int *m){assert(!*m);*m=1;}
static void mutex_unlock(int *m){assert(*m);*m=0;}
static void *dev_get_drvdata(struct device *d){(void)d;return &conn;}
static struct y2_conn *hci_get_drvdata(struct hci_dev *h){return h->c;}
static void cancel_work_sync(int *w){(void)w;}
static void skb_queue_purge(int *q){(void)q;}
static void kfree_skb(void *s){(void)s;}
static int power_on(struct y2_conn *c){rec(POWER_ON);c->powered=c->dma_active=c->full_stp=true;return 0;}
static int power_off(struct y2_conn *c){rec(POWER_OFF);c->powered=c->dma_active=c->full_stp=false;return 0;}
static int y1_wmt_sleep(struct y2_conn *c){assert(c->powered && c->dma_active);rec(SLEEP);return 0;}
static int y2_conn_rail(struct y2_conn *c,unsigned rail,bool on){(void)c;assert(rail==2);rec(on?PA_ON:PA_OFF);return 0;}
static int y2_wmt_function(struct y2_conn *c,unsigned f,bool on){(void)c;assert(f==Y2_CONN_BT);rec(on?FUNC_ON:FUNC_OFF);return 0;}
static void y2_conn_failed(struct y2_conn *c,int e){c->failure=e;}
static int runtime_resume(struct device *d);
static int runtime_suspend(struct device *d);
static int pm_runtime_resume_and_get(struct device *d){return runtime_resume(d);}
static void pm_runtime_put(struct device *d){runtime_suspend(d);}
static void pm_runtime_mark_last_busy(struct device *d){(void)d;}
static void pm_runtime_put_autosuspend(struct device *d){runtime_suspend(d);}
""" + body + r"""
int main(void){
 struct hci_dev h={&conn}; conn.dev=&dev; conn.activated=true;
 /* hci0 up: power-on with the WMT boot, the BT PA rail, WMT function on. */
 assert(!open(&h) && conn.hci_open && conn.functions==1);
 /* hci0 down: function off, PA rail off, then (autosuspend) WMT sleep and the power-off. */
 assert(!close(&h) && !conn.hci_open && !conn.functions && !conn.powered);
 static const unsigned expect[]={POWER_ON,PA_ON,FUNC_ON,FUNC_OFF,PA_OFF,SLEEP,POWER_OFF};
 assert(n==7 && !memcmp(log_,expect,sizeof(expect)));
 /* A failed transport powers off without the sleep command. */
 n=0; assert(!open(&h)); conn.failure=-EIO; conn.hci_open=false;
 assert(!close(&h) && log_[n-1]==POWER_OFF && log_[n-2]==PA_OFF);
 /* Not activated (no factory record): hci never powers anything. */
 n=0; conn.failure=0; conn.activated=false; assert(open(&h)==-EHOSTDOWN && !n);
}
""")


class Y1WifiW6(unittest.TestCase):
    """W6 (docs/planning/Y1-WIFI-PLAN.md §14): S41's late Wi-Fi record and the firmware scan, the MTP hook."""

    SCRIPT = (ROOT / 'initramfs/y1-bluetooth/etc/init.d/S41y2-connectivity').read_text()

    def function(self, name):
        return re.search(rf'^{name}\(\) \{{\n.*?^\}}\n', self.SCRIPT, re.M | re.S)[0]

    def wifi_record(self, directory, child=False, record=b'r' * 512, mode=0o600):
        base = Path(directory)
        core = base / 'core'
        core.mkdir()
        (core / 'wifi_factory').write_bytes(b'')
        if child:
            (core / 'y2-mt6582-wifi').mkdir()
        path = base / 'wifi-record'
        path.write_bytes(record)
        path.chmod(mode)
        # ls -ln names uid 0 only for root: stand in for it with the caller's uid.
        body = self.function('wifi_record').replace('"-rw------- 0 512"', f'"-rw------- {__import__("os").getuid()} 512"')
        run = subprocess.run(['sh', '-c', f'C={core}\nWRECORD={path}\n{body}wifi_record'], capture_output=True)
        return run.returncode, (core / 'wifi_factory').read_bytes()

    def test_the_record_goes_to_the_driver_once_while_there_is_no_wifi_device(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(self.wifi_record(d), (0, b'r' * 512))
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(self.wifi_record(d, child=True), (0, b''))  # registered already: nothing
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(self.wifi_record(d, mode=0o644), (0, b''))
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(self.wifi_record(d, record=b'r' * 511), (0, b''))

    def firmware(self, directory, status, copy):
        base = Path(directory)
        (base / 'bin').mkdir()
        (base / 'bin/y2-platform').write_text(f'#!/bin/sh\necho \'{status}\'\n')
        (base / 'bin/start-stop-daemon').write_text(f'#!/bin/sh\necho "$@" > {base}/started\n')
        for tool in ('y2-platform', 'start-stop-daemon'):
            (base / 'bin' / tool).chmod(0o755)
        inbox = base / 'Firmware'
        inbox.mkdir()
        if copy:
            (inbox / 'system.img').write_bytes(b'x')
        run = subprocess.run(['sh', '-c', f'PATH={base}/bin:$PATH\nINBOX={inbox}\nWAIT={base}/wait.pid\n'
                              f'{self.function("firmware")}firmware'], capture_output=True)
        started = base / 'started'
        return run.returncode, started.read_text() if started.exists() else None

    def test_the_scan_runs_while_missing_or_while_a_copy_waits(self):
        ready, missing = '{"state":"Ready"}', '{"state":"Missing"}'
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(self.firmware(d, ready, copy=False), (0, None))
        for status, copy in ((ready, True), (missing, False), (missing, True)):
            with tempfile.TemporaryDirectory() as d:
                code, started = self.firmware(d, status, copy)
                self.assertIn('firmware-wait', started)
                self.assertIn('-S -q -b -m -p', started)
        wait = self.SCRIPT.split(' firmware-wait)')[1].split(';;\nesac')[0]
        self.assertIn('[ -f "$INBOX/system.img" ] && [ "$n" -lt 60 ] || break', wait)

    def test_the_inbox_is_the_configured_one_and_mtp_judges_a_finished_copy(self):
        config = json.loads((ROOT / 'initramfs/y1-bluetooth/etc/y2linux/radio-firmware.json').read_text())
        self.assertIn(f"INBOX={config['inbox']}\n", self.SCRIPT)
        # With the built-in card as Internal Storage (S03y1-media) the inbox is its Firmware folder, the one MTP shows.
        self.assertIn('\tINBOX=/media/internal/music/Firmware\n', self.SCRIPT)
        mtp = (ROOT / 'initramfs/y1-mtp/etc/init.d/S45y1-mtp').read_text()
        self.assertIn(f"MUSIC={config['inbox'].removesuffix('/Firmware')}\n", mtp)
        self.assertIn('[ -f "$MUSIC/Firmware/system.img" ] && /etc/init.d/S41y2-connectivity firmware', mtp)
        self.assertIn(' firmware) firmware;;', self.SCRIPT)

    def test_the_service_daemon_is_left_to_s42(self):
        # R4b (docs/planning/Y1-RECOVERY-PLAN.md): S42y2-readiness starts the service daemon with or without
        # Wi-Fi (tests/test_y1_update_service.py); a restart of this script after a firmware import must not
        # stop it.
        self.assertNotIn('readiness-supervis', self.SCRIPT)
        self.assertNotIn('service-policy', self.SCRIPT)
        self.assertIn("service daemon (its network and update parts) is S42y2-readiness's", self.SCRIPT)

    def test_the_driver_takes_a_late_record_once_under_the_recovery_lock(self):
        core = (Y1 / 'core.c').read_text()
        write = core[core.index('static ssize_t wifi_factory_write'):core.index('static const struct bin_attribute bin_attr_wifi_factory')]
        self.assertIn('if (c->activated && (c->wifi_ready || c->wifi || c->removing || c->recovering)) ret=-EBUSY;', write)
        self.assertLess(write.index('mutex_lock(&c->recovery_lock);'), write.index('register_wifi(c)'))
        self.assertLess(write.index('register_wifi(c)'), write.index('mutex_unlock(&c->recovery_lock);'))
        activate = core[core.index('static ssize_t activate_store'):core.index('static ssize_t status_show')]
        self.assertLess(activate.index('mutex_lock(&c->recovery_lock);'), activate.index('register_wifi(c)'))
        self.assertIn('out:\n\tmutex_unlock(&c->recovery_lock);', activate)


if __name__ == '__main__':
    unittest.main()


class Y1WifiW3(unittest.TestCase):
    """W3 (docs/planning/Y1-WIFI-PLAN.md): the thirty-fifth tree and fragment, S41's Wi-Fi record."""

    def wifi_compose(self, directory, defaults=None, record=None, wifi_build=True):
        script = (ROOT / 'initramfs/y1-bluetooth/etc/init.d/S41y2-connectivity').read_text()
        body = re.search(r'^wifi_compose\(\) \{\n.*?^\}\n', script, re.M | re.S)[0]
        base = Path(directory)
        (base / 'firmware').mkdir(exist_ok=True)
        (base / 'core').mkdir(exist_ok=True)
        if wifi_build:
            (base / 'core/wifi_factory').write_bytes(b'')
        if defaults is not None:
            (base / 'firmware/WIFI.defaults').write_bytes(defaults)
        path = base / 'wifi-record'
        if record is not None:
            path.write_bytes(record)
        run = subprocess.run(['sh', '-c', f'C={base / "core"}\nFIRMWARE={base / "firmware"}\nWRECORD={path}\n'
                              f'{body}wifi_compose'], capture_output=True)
        return run.returncode, path.read_bytes() if path.exists() else None, path

    def test_a_new_wifi_record_is_the_default_with_a_unicast_local_address(self):
        defaults = bytearray(range(256)) * 2  # synthetic settings, never the stock bytes
        defaults[4:10] = bytes(6)
        seen = set()
        for _ in range(8):
            with tempfile.TemporaryDirectory() as d:
                code, record, path = self.wifi_compose(d, bytes(defaults))
                self.assertEqual(code, 0)
                self.assertEqual(len(record), 512)
                self.assertEqual(record[:4] + record[10:], bytes(defaults[:4] + defaults[10:]))
                self.assertEqual(record[4] & 3, 2)  # unicast, locally administered
                self.assertEqual(path.stat().st_mode & 0o777, 0o600)
                self.assertFalse(path.with_name('wifi-record.new').exists())
                seen.add(record[4:10])
        self.assertGreater(len(seen), 1)

    def test_nothing_without_a_wifi_build_or_the_defaults_and_a_record_is_kept(self):
        defaults = bytes(512)
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(self.wifi_compose(d, defaults, wifi_build=False)[:2], (0, None))
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(self.wifi_compose(d)[:2], (0, None))
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(self.wifi_compose(d, bytes(511))[:2], (0, None))
        kept = bytes(range(256)) * 2
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(self.wifi_compose(d, defaults, kept)[:2], (0, kept))

    def test_the_tree_carries_the_wifi_window_and_interrupt(self):
        # W3 (docs/planning/Y1-WIFI-PLAN.md): MediaTek's MT6572 CONN_HIF_BASE and MT_WF_HIF_IRQ_ID.
        tree = (ROOT / 'kernel/dts/innioasis-y1.dts').read_text()
        node = tree[tree.index('connectivity@18070000 {'):]
        node = node[:node.index('};')]
        self.assertIn('<0x11000400 0x80>, <0x180f0000 0x5c>;', node)
        self.assertIn('reg-names = "mcu", "conn-emi", "btif", "tx-dma", "rx-dma", "wifi";', node)
        self.assertIn('<GIC_SPI 123 IRQ_TYPE_LEVEL_LOW>;', node)
        self.assertIn('interrupt-names = "btif", "tx-dma", "rx-dma", "wake", "wifi";', node)

    def test_the_fragment_carries_the_wifi_lines_and_the_y1_initramfs(self):
        text = (ROOT / 'kernel/config/y1-production.config').read_text()
        lines = {l for l in text.splitlines() if l.startswith('CONFIG_') or l.startswith('# CONFIG_')}
        self.assertLessEqual({
            'CONFIG_WIRELESS=y', 'CONFIG_CFG80211=y', 'CONFIG_CFG80211_REQUIRE_SIGNED_REGDB=y',
            'CONFIG_CFG80211_USE_KERNEL_REGDB_KEYS=y', 'CONFIG_BT_INNIOASIS_Y1_WIFI=y', 'CONFIG_PACKET=y'}, lines)
        self.assertNotIn('# CONFIG_WIRELESS is not set', lines)
        self.assertIn('CONFIG_INITRAMFS_SOURCE="/project/initramfs/y1-production.list"', lines)


class Y1WifiW4(unittest.TestCase):
    """W4 (docs/planning/Y1-WIFI-PLAN.md): the Y1-only passive scan in the shared fullmac, the Y2 unchanged."""

    WIFI = ROOT / 'kernel/platform/connectivity/wifi'

    def test_the_flag_is_the_y1_options_only(self):
        makefile = (self.WIFI / 'Makefile').read_text()
        self.assertIn('ccflags-$(CONFIG_BT_INNIOASIS_Y1_WIFI) += -DY2_WIFI_PASSIVE_SCAN\n', makefile)
        self.assertEqual(makefile.count('Y2_WIFI_PASSIVE_SCAN'), 1)
        hif = (self.WIFI / 'hif.h').read_text()
        self.assertIn('#define Y2_WIFI_SCAN_REQUEST(glue, request) ((void)0)', hif)
        self.assertIn('#define Y2_WIFI_SCAN_TYPE(adapter, ais, msg) ((void)0)', hif)
        self.assertIn('(request)->n_ssids == 0', hif)
        self.assertIn('(ais)->eCurrentState == AIS_STATE_SCAN || (ais)->eCurrentState == AIS_STATE_ONLINE_SCAN', hif)

    def test_hooks_end_existing_vendor_lines(self):
        # Appended, never inserted: the vendor files keep every line number (their __LINE__ constants and so the
        # Y2's objects; W4 build: Image a68fdffd, vmlinux b5ae438d, all 48 objects identical).
        cases = {'os/linux/gl_cfg80211.c': ('    kalMemZero(&rScanRequest, sizeof(PARAM_SCAN_REQUEST_EXT_T)); '
                                            'Y2_WIFI_SCAN_REQUEST(prGlueInfo, request);', 536),
                 'mgmt/ais_fsm.c': ('            prScanReqMsg->eScanType         = SCAN_TYPE_ACTIVE_SCAN; '
                                    'Y2_WIFI_SCAN_TYPE(prAdapter, prAisFsmInfo, prScanReqMsg);', 2297)}
        for name, (line, number) in cases.items():
            lines = (self.WIFI / name).read_text().split('\n')
            self.assertEqual(lines[number - 1], line, name)
            self.assertEqual(sum('Y2_WIFI_SCAN_' in l for l in lines), 1, name)
        self.assertNotIn('PASSIVE', (self.WIFI / 'include/wlan_oid.h').read_text().split('SCAN_REQUEST_EXT_T')[1][:200])


class Y1RecordB7(unittest.TestCase):
    """B7: S41 composes the record once from the platform's BT.defaults and a random address."""

    def compose(self, directory, defaults=None, record=None):
        script = (ROOT / 'initramfs/y1-bluetooth/etc/init.d/S41y2-connectivity').read_text()
        body = re.search(r'^compose\(\) \{\n.*?^\}\n', script, re.M | re.S)[0]
        firmware = Path(directory) / 'firmware'
        firmware.mkdir(exist_ok=True)
        if defaults is not None:
            (firmware / 'BT.defaults').write_bytes(defaults)
        path = Path(directory) / 'bt-record'
        if record is not None:
            path.write_bytes(record)
        run = subprocess.run(['sh', '-c', f'FIRMWARE={firmware}\nRECORD={path}\n{body}compose'],
                             capture_output=True)
        return run.returncode, path.read_bytes() if path.exists() else None, path

    def test_a_new_record_is_the_defaults_behind_a_unicast_local_address(self):
        defaults = bytes(6) + bytes(range(100, 126))  # synthetic settings, never the stock bytes
        seen = set()
        for _ in range(8):
            with tempfile.TemporaryDirectory() as d:
                code, record, path = self.compose(d, defaults)
                self.assertEqual(code, 0)
                self.assertEqual(len(record), 32)
                self.assertEqual(record[6:], defaults[6:])
                self.assertEqual(record[0] & 3, 2)  # unicast, locally administered
                self.assertEqual(path.stat().st_mode & 0o777, 0o600)
                self.assertFalse(path.with_name('bt-record.new').exists())
                seen.add(record[:6])
        self.assertGreater(len(seen), 1)

    def test_an_existing_record_is_kept_and_bad_defaults_write_nothing(self):
        owner = bytes(range(32))
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(self.compose(d, bytes(32), owner)[:2], (0, owner))
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(self.compose(d)[:2], (1, None))
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(self.compose(d, bytes(31))[:2], (1, None))

    def test_the_radio_firmware_set_is_the_inventory_and_the_stock_default_record(self):
        config = json.loads((ROOT / 'initramfs/y1-bluetooth/etc/y2linux/radio-firmware.json').read_text())
        # The import is the Y1's own system.img: the optional Wi-Fi firmware comes from the Y2 package (W6).
        wanted = {f['filename']: (f['bytes'], f['sha256']) for f in INVENTORY['files']
                  if f['filename'] != 'modem_1_2g_n.img' and not f.get('optional')}
        self.assertEqual({f['name']: (f['size'], f['sha256']) for f in config['files']}, wanted)
        header = (Y1 / 'conn.h').read_text()
        self.assertIn('#define Y2_CONN_FW "mt6572/"', header)
        self.assertEqual(config['target'], '/data/firmware/mt6572')
        self.assertEqual(config['inbox'], '/data/music/Firmware')  # S45y1-mtp's one storage, Music
        bt, wifi = config['defaults']
        # The stock record starts with the generic MT6572 address (y1-conn-policy.h's reject hash).
        self.assertEqual((bt['offset'], bt['size'], bt['expect'], bt['clear']), (0x3004, 32, '000046657201', 6))
        self.assertNotIn('library', bt)
        # W6: the Wi-Fi default from the Y1's own libcustom_nvram.so (the inventory's record and library).
        default = INVENTORY['wifi_default_record']
        self.assertEqual((wifi['name'], wifi['size'], wifi['sha256']),
                         (default['filename'], default['bytes'], default['sha256']))
        self.assertEqual((wifi['offset'], wifi['clear'], wifi['library']), (0x3cf4, 0, '/lib/libcustom_nvram.so'))
        self.assertIn('file offset 0x3cf4', default['source'])
        (library,) = config['libraries']
        self.assertEqual(library['name'], wifi['library'])
        self.assertIn(f"({library['size']} bytes, sha256 {library['sha256']})", default['source'])
        # The supplement is the inventory's optional file: the Y2 package's Wi-Fi firmware.
        optional = {f['filename']: (f['bytes'], f['sha256']) for f in INVENTORY['files'] if f.get('optional')}
        self.assertEqual({f['name']: (f['size'], f['sha256']) for f in config['supplement']}, optional)
        self.assertIn('Y2 package', config['note'])
        self.assertIs(config['calibration'], False)  # no MD1: the monitor never starts y2-calibration
        self.assertEqual(config['patches'], [1])
