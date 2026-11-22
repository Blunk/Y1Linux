"""Build the Innioasis Y1's Y2DATA seed image for USRDATA (docs/planning/Y1-DATA-PLAN.md, stage B0) into a fresh
directory below out/; never access a device. Like the Y2's (tools/production/package.py) it is created offline and
flashed once: ext4 labelled Y2DATA with a fixed Y1 UUID over all of USRDATA, the schema file, the owner's
authorised SSH key, empty host-key and music directories, and Reborn's seeded settings (the speaker off; the screen
timeout is Reborn's default, as on the Y2, since a centre press wakes the dark screen). Owned by root without host privilege: debugfs sets the
owners after mkfs. The power daemon's override files are never seeded (they would replace the Y1's in /etc).

    python3 -m tools.build.y1_data out/y1-data-1
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

USRDATA_PHYSICAL = 0x4cf40000      # scatter 0x4d540000 - 0x600000
USRDATA_BYTES = 0x80000000
LABEL = 'Y2DATA'
UUID = '79314c69-6e75-4801-8000-000000000102'
KEY = Path('out/y1-ssh/id_ed25519.pub')
SESSION = {'session_schema': 3, 'settings': {'speaker': False}}
DIRECTORIES = {'ssh': 0o700, 'ssh/authorized_keys.d': 0o700, 'ssh/host-keys': 0o700, 'music': 0o700,
               'reborn': 0o700, 'reborn/state': 0o700, 'system': 0o755, 'system/platform': 0o700}
FILES = {'.y2data-schema': 0o644, 'ssh/authorized_keys.d/authorized_keys': 0o600,
         'reborn/state/session.json': 0o600}


def require(condition, what):
    if not condition:
        raise SystemExit('y1_data: ' + what)


FAKE_TIME = dict(os.environ, E2FSPROGS_FAKE_TIME='1793836800')  # reproducible inode and superblock times


def debugfs(image, request, write=False):
    command = ['debugfs'] + (['-w'] if write else []) + ['-R', request, str(image)]
    return subprocess.run(command, env=FAKE_TIME, check=True, capture_output=True, text=True).stdout


def seed(root, key):
    for name, mode in DIRECTORIES.items():
        (root / name).mkdir(parents=True)
        (root / name).chmod(mode)
    (root / '.y2data-schema').write_text('1\n')
    (root / 'ssh/authorized_keys.d/authorized_keys').write_bytes(key)
    (root / 'reborn/state/session.json').write_text(json.dumps(SESSION) + '\n')
    for name, mode in FILES.items():
        (root / name).chmod(mode)
    for path in [root, *root.rglob('*')]:        # mkfs -d copies the seed's times
        os.utime(path, (1793836800, 1793836800))


def build(out, key):
    root, image = out / 'seed', out / 'Y2DATA-y1.img'
    seed(root, key)
    with image.open('wb') as f:
        f.truncate(USRDATA_BYTES)
    subprocess.run(['mkfs.ext4', '-q', '-F', '-L', LABEL, '-U', UUID, '-O', '^64bit,^metadata_csum_seed',
                    '-E', 'lazy_itable_init=0,lazy_journal_init=0,hash_seed=' + UUID, '-d', str(root), str(image)],
                   env=FAKE_TIME, check=True)
    for name in ['', *DIRECTORIES, *FILES]:
        path = '/' + name
        for field, value in (('uid', '0'), ('gid', '0'), ('ctime', '@1793836800')):
            debugfs(image, f'sif {path} {field} {value}', write=True)
    return image


def check(image, key):
    require(image.stat().st_size == USRDATA_BYTES, 'image size is USRDATA')
    subprocess.run(['e2fsck', '-fn', str(image)], check=True, capture_output=True)
    header = subprocess.run(['dumpe2fs', '-h', str(image)], check=True, capture_output=True, text=True).stdout
    require(f'Filesystem volume name:   {LABEL}' in header, 'label')
    require(f'Filesystem UUID:          {UUID}' in header, 'UUID')
    require(debugfs(image, 'cat /.y2data-schema') == '1\n', 'schema')
    require(debugfs(image, 'cat /ssh/authorized_keys.d/authorized_keys').encode() == key, 'authorised key')
    for name in ['', *DIRECTORIES, *FILES]:
        stat = debugfs(image, 'stat /' + name)
        require('User:     0   Group:     0' in stat, 'root ownership of /' + name)
    for name in ('system/platform/battery-profile.json', 'system/platform/power-policy.json'):
        require('File not found' in subprocess.run(['debugfs', '-R', 'stat /' + name, str(image)],
                                                   capture_output=True, text=True).stderr, 'no power override')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('out', type=Path)
    out = parser.parse_args().out
    require(out.resolve().is_relative_to(Path('out').resolve()), 'output below out/')
    require(not out.exists(), 'a fresh output directory is required')
    require(KEY.exists(), 'the owner key ' + str(KEY))
    key = KEY.read_bytes()
    out.mkdir(parents=True)
    image = build(out, key)
    check(image, key)
    digest = hashlib.sha256(image.read_bytes()).hexdigest()
    result = {'image': image.name, 'bytes': USRDATA_BYTES, 'sha256': digest, 'label': LABEL, 'uuid': UUID,
              'usrdata_physical_start': hex(USRDATA_PHYSICAL), 'scatter_start': hex(USRDATA_PHYSICAL + 0x600000)}
    (out / 'y1-data.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
