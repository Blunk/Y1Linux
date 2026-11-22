"""The Innioasis Y1's release build (release.py build --device y1; docs/planning/Y1-RECOVERY-PLAN.md §13).

It makes what a Y2 release has, for the Y1: the signed over-the-air update, the images mtkclient writes (as a
first-install zip) and the corresponding sources, with the Y2 release's checks plus the Y1's own. Never touches
GitHub or a device.

- Y2ROOT in the workspace out/build, then the Y1 BOOTIMG and RECOVERY from it (tools/build/y1-build.sh) in
  out/release-<label>/boot.
- Checks: the shared ones (release.checks), the Y1 tests, the privacy scan of the root image and of the
  recovery's initramfs files, and a firmware scan of every image against the player's own radio firmware
  (a copy of /data/firmware/mt6572, by default ~/.local/share/y1linux/stock-firmware).
- The signing key must be the one the build trusts: its public half is the active release key of
  tools/update/trust.json, and the signed manifest verifies against the trust store inside the root image and
  inside the recovery's initramfs, so the release can update the players that run it.
- Assets: manifest.json, manifest.sig, rootfs.ext4.gz, bootimg.img.gz, recovery.img.gz,
  Y1Linux-<version>-first-install.zip (rootfs.ext4, BOOTIMG.img, RECOVERY.img, README.txt, SHA256SUMS), the
  source archives (release.source_assets) and SHA256SUMS over all of them. The data image (USRDATA) is not one:
  it carries the owner's SSH key, so every owner builds it (docs/INSTALL.md).
"""
import ctypes
import json
import os
import shutil
import stat
import subprocess
import time
from pathlib import Path

from tools.production import release as base
from tools.update import package as ota_package

PROJECT, OUT = base.PROJECT, base.OUT
TRUST = PROJECT / 'tools/update/trust.json'
FIRMWARE = Path.home() / '.local/share/y1linux/stock-firmware'
BUILD_ID_PREFIX = 'Y1LINUX-'
REPO_URL = f'https://github.com/{base.REPO}'
INSTALLER_TRUST = 'installer/files/etc/y2linux/update-trust.json'
README = """Y1Linux {version} for the Innioasis Y1: the images mtkclient writes.

Install, the eMMC backup and the data image (USRDATA, which you build yourself):
{repo}/blob/{tag}/docs/INSTALL.md

    sha256sum -c SHA256SUMS
    python mtk.py r RECOVERY recovery-stock.img   # keep it: your way back to the stock recovery
    python mtk.py w ANDROID rootfs.ext4
    python mtk.py w BOOTIMG BOOTIMG.img
    python mtk.py w RECOVERY RECOVERY.img

Never write USRDATA over an installed Y1Linux: it wipes settings, library, music and the radio firmware.
From then on the player updates itself over Wi-Fi (Settings > System > Software Update).
"""


def tag(version):
    return f'y1linux-{version}'


def release_key(trust, key_id):
    """The active release key `key_id` of a trust store (parsed JSON)."""
    keys = [key for key in trust['keys'] if key['id'] == key_id and key['status'] == 'active'
            and key['purpose'] == 'release']
    if len(keys) != 1:
        raise SystemExit(f'trust store: no single active release key {key_id}')
    return keys[0]


def check_key(private, key_id):
    """The private key (libsodium: seed, then the public key) is the trusted release key `key_id`."""
    try:
        fd = os.open(private, os.O_RDONLY | os.O_NOFOLLOW)
    except OSError as error:
        raise SystemExit(f'signing key {private}: {error.strerror}') from None
    with os.fdopen(fd, 'rb') as stream:
        mode = os.fstat(stream.fileno()).st_mode
        secret = stream.read(65)
    if not stat.S_ISREG(mode) or mode & 0o077 or len(secret) != 64:
        raise SystemExit(f'signing key {private}: expected a mode-0600 libsodium Ed25519 key')
    public = release_key(json.loads(TRUST.read_text()), key_id)['public_key_hex']
    if secret[32:].hex() != public:
        raise SystemExit(f'signing key {private} is not {key_id} of {TRUST.name}')


def verify(manifest, signature, trust, key_id):
    """Whether `signature` over the manifest bytes verifies with the release key `key_id` of `trust`."""
    public = bytes.fromhex(release_key(trust, key_id)['public_key_hex'])
    lib = ota_package.sodium()
    lib.crypto_sign_verify_detached.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_ulonglong,
                                                ctypes.c_char_p]
    return len(signature) == 64 and lib.crypto_sign_verify_detached(signature, manifest, len(manifest), public) == 0


def image_file(image, path):
    """A file of an ext4 image, read with debugfs (no mount)."""
    result = subprocess.run(['debugfs', '-R', f'cat {path}', str(image)], capture_output=True, check=True)
    return result.stdout


def check_sequence(versions, trust, key_id):
    if not versions['build_id'].startswith(BUILD_ID_PREFIX):
        raise SystemExit(f'build ID {versions["build_id"]} is not a Y1 one: run release.py bump --device y1')
    key = release_key(trust, key_id)
    if not key['minimum_sequence'] <= versions['update_sequence'] <= key['maximum_sequence']:
        raise SystemExit(f'update sequence {versions["update_sequence"]} is outside {key_id}\'s range')


def firmware_files(folder):
    files = {path.name: path.read_bytes() for path in sorted(Path(folder).iterdir()) if path.is_file()}
    if not files:
        raise SystemExit(f'firmware scan: copy the player\'s /data/firmware/mt6572 to {folder}')
    return files


def first_install(release, assets, version, images):
    """Y1Linux-<version>-first-install.zip: the three images, their SHA256SUMS and how to write them."""
    name = f'Y1Linux-{version}-first-install'
    staging = release / name
    staging.mkdir()
    for target, source in images.items():
        shutil.copyfile(source, staging / target)
    (staging / 'README.txt').write_text(README.format(version=version, repo=REPO_URL, tag=tag(version)))
    (staging / 'SHA256SUMS').write_text(''.join(f'{base.sha256(staging / target)}  {target}\n'
                                                for target in sorted(images)))
    subprocess.run(['zip', '-q', '-r', '-6', assets / f'{name}.zip', name], cwd=release, check=True)
    shutil.rmtree(staging)


def build(args):
    check_key(args.key, base.KEY_ID)
    firmware = firmware_files(args.firmware)
    release, steps = base.start(args.label)
    started = time.monotonic()
    folder = base.WORKSPACE
    fresh = base.build_root(steps, args.clean)
    versions = json.loads((folder / 'versions.json').read_text())
    version = versions['release_version']
    trust = json.loads(TRUST.read_text())
    check_sequence(versions, trust, base.KEY_ID)
    root = (folder / 'buildroot/images/rootfs.ext4').resolve()
    boot = release / 'boot'
    tests = sorted(f'tests.{path.stem}' for path in (PROJECT / 'tests').glob('test_y1_*.py'))
    host = dict(os.environ, PYTHONPATH=f'{PROJECT}{os.pathsep}{PROJECT / "tests"}')
    ran = base.checks(steps, folder, [
        [('y1-boot', ['python3', 'tools/build/run.py', '--manifest', 'kernel/patches-y1/manifest.json',
                      '--output', boot.relative_to(PROJECT), '--', 'sh', '/project/tools/build/y1-build.sh',
                      f'/project/{folder.relative_to(PROJECT)}']),
         ('recovery-privacy', ['python3', 'tools/production/privacy.py', '--tree', boot / 'installer/files'])],
        [('y1-tests', ['python3', '-m', 'unittest', *tests], host)],
        [('root-privacy', ['python3', 'tools/production/privacy.py', '--root-image', root])],
    ])

    ota = release / 'ota'
    steps.run('ota-sign', ['python3', 'tools/update/package.py', 'package', '--device', 'y1', '--image', root,
                           '--bootimg', boot / 'BOOTIMG.img', '--recovery', boot / 'RECOVERY.img',
                           '--versions', folder / 'versions.json', '--sequence', versions['update_sequence'],
                           '--rollback-allowed', '--key-id', base.KEY_ID, '--private', args.key,
                           '--notes', args.notes], log='ota-release.json')
    manifest, signature = (ota / 'manifest.json').read_bytes(), (ota / 'manifest.sig').read_bytes()
    stores = {'root image': json.loads(image_file(root, '/etc/y2linux/update-trust.json')),
              'recovery': json.loads((boot / INSTALLER_TRUST).read_text())}
    for where, store in stores.items():
        if not verify(manifest, signature, store, base.KEY_ID):
            raise SystemExit(f'the signed manifest does not verify with the trust store of the {where}')

    moment = time.monotonic()
    images = {'rootfs.ext4': root, 'BOOTIMG.img': boot / 'BOOTIMG.img', 'RECOVERY.img': boot / 'RECOVERY.img'}
    scan = {name: hit for name, hit in zip(images, base.firmware_free(list(images.values()), firmware).values())}
    steps.durations['firmware-scan'] = round(time.monotonic() - moment, 1)
    if any(scan.values()):
        raise SystemExit(f'MediaTek firmware found in {[name for name, hit in scan.items() if hit]}')

    moment = time.monotonic()
    assets = release / 'assets'
    assets.mkdir()
    for path in ota.iterdir():
        shutil.copy2(path, assets / path.name)
    first_install(release, assets, version, images)
    base.source_assets(assets, release, folder, versions, 'Y1Linux')
    sums = ''.join(f'{base.sha256(path)}  {path.name}\n' for path in sorted(assets.iterdir())
                   if path.name != 'SHA256SUMS')
    (assets / 'SHA256SUMS').write_text(sums)
    steps.durations['assets'] = round(time.monotonic() - moment, 1)
    steps.durations['total'] = round(time.monotonic() - started, 1)

    kernel = boot / 'kernel/include/config/kernel.release'
    summary = {'label': args.label, 'device': 'y1', 'tag': tag(version), 'release_version': version,
               'build_id': versions['build_id'], 'kernel_version': kernel.read_text().strip(),
               'update_sequence': versions['update_sequence'], 'key_id': base.KEY_ID,
               'linux_commit': versions['build_git_commit'], 'reborn_commit': versions['reborn_source_commit'],
               'build': str(folder.relative_to(PROJECT)), 'clean_build': fresh, 'production_tests': ran,
               'firmware_found': scan,
               'trust_verified': sorted(stores),
               'images': {name: base.sha256(path) for name, path in images.items()},
               'assets': {path.name: path.stat().st_size for path in sorted(assets.iterdir())},
               'durations_s': steps.durations}
    base.finish(release, summary, args)
