#!/usr/bin/env python3
"""Run a command in the locked, networkless, device-isolated build userspace."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

PROJECT = Path(__file__).resolve().parents[2]
REBORN = PROJECT.parent / 'Y2Reborn'
sys.path.insert(0, str(PROJECT))


def prepare_overlay(project, spec, patches=None):
    """Publish verified immutable overlay bytes; never rewrite a bound inode. Patches are read from
    the manifest's directory, by default the Y2 set in kernel/patches."""
    patches = project / 'kernel/patches' if patches is None else patches
    original = (project / '.cache/sources/linux-6.18' / spec['path']).read_bytes()
    if hashlib.sha256(original).hexdigest() != spec['base_sha256']:
        raise ValueError('Reviewed overlay base mismatch: ' + spec['path'])
    overlay = project / '.cache/overlays' / spec['result_sha256']
    overlay.parent.mkdir(parents=True, exist_ok=True)
    if overlay.exists():
        if hashlib.sha256(overlay.read_bytes()).hexdigest() != spec['result_sha256']:
            raise ValueError('Existing overlay cache mismatch: ' + spec['path'])
        return overlay
    patch = (patches / spec['patch']).read_bytes()
    with tempfile.NamedTemporaryFile(dir=overlay.parent, prefix='.overlay-', delete=False) as f:
        temporary = Path(f.name)
        f.write(original)
    try:
        subprocess.run(['patch', '--batch', '--fuzz=0', str(temporary)], input=patch, check=True,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if hashlib.sha256(temporary.read_bytes()).hexdigest() != spec['result_sha256']:
            raise ValueError('Reviewed overlay result mismatch: ' + spec['path'])
        os.replace(temporary, overlay)
    finally:
        temporary.unlink(missing_ok=True)
    return overlay


def touch_changed_overlays(output, specs, files):
    """Give an overlay that changed for its path since this build directory last used it a
    new mtime. The cache is content-addressed: returning to an earlier overlay (a reverted
    patch) brought back an older file than the object built from its successor, and make
    kept the stale object (test build 6452 still carried a backed-out mtk-mmsys.c change)."""
    state = output / '.y2-overlays.json'
    try:
        previous = json.loads(state.read_text())
    except (OSError, ValueError):
        previous = None
    current = {spec['path']: spec['result_sha256'] for spec in specs}
    for spec, overlay in zip(specs, files):
        # No record yet (first build with this rule): no object can be trusted.
        if previous is None or previous.get(spec['path']) != spec['result_sha256']:
            os.utime(overlay)
    temporary = state.with_name(state.name + '.tmp')
    temporary.write_text(json.dumps(current, indent=1, sort_keys=True) + '\n')
    os.replace(temporary, state)


def verify_added(project, specs):
    """New upstream-path files (a manifest's "added" list) with their reviewed source hashes."""
    for spec in specs:
        if hashlib.sha256((project / spec['source']).read_bytes()).hexdigest() != spec['sha256']:
            raise ValueError('Reviewed added file mismatch: ' + spec['source'])
        if (project / '.cache/sources/linux-6.18' / spec['path']).exists():
            raise ValueError('Added file exists upstream (use an overlay): ' + spec['path'])


def expose_directories(source, paths):
    """bwrap arguments that turn each upstream directory into a private tmpfs listing its original
    children, so files can be added to it without creating anything in the locked tree. Parents
    come first: a deeper directory is then laid out inside its already-exposed parent."""
    arguments = []
    for directory in sorted({str(Path(path).parent) for path in paths}, key=lambda d: (d.count('/'), d)):
        arguments += ['--tmpfs', '/src/' + directory]
        for child in sorted((source / directory).iterdir()):
            arguments += ['--ro-bind', str(child), f'/src/{directory}/{child.name}']
    return arguments


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', default='out/build', help='project-relative build directory')
    parser.add_argument('--manifest', default='kernel/patches/manifest.json',
                        help='project-relative overlay manifest (the Y2 set by default)')
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command:
        parser.error('a command is required')
    lock_path = PROJECT / 'tools/build/inputs.lock.json'
    lock = json.loads(lock_path.read_text())
    extra_lock = PROJECT / 'tools/build/connectivity-host.lock.json'
    fingerprint = hashlib.sha256(lock_path.read_bytes()+extra_lock.read_bytes()).hexdigest()
    rootfs = PROJECT / '.cache' / ('environment-m5-'+fingerprint[:12])
    marker = rootfs / '.y2-build-lock'
    if not marker.exists() or marker.read_text().strip() != fingerprint:
        raise SystemExit('Run tools/build/prepare.py first; environment lock mismatch')
    output = (PROJECT / args.output).resolve()
    if not output.is_relative_to(PROJECT / 'out'):
        raise SystemExit('Build output must be below project/out')
    output.mkdir(parents=True, exist_ok=True)
    # Each existing-file overlay requires exact base/result hashes and a zero-fuzz patch.
    manifest_path = PROJECT / args.manifest
    manifest = json.loads(manifest_path.read_text())
    specs = manifest['overlays']
    added = manifest.get('added', [])
    verify_added(PROJECT, added)
    overlays = []
    for spec in specs:
        overlays.append((prepare_overlay(PROJECT,spec,manifest_path.parent), spec['path']))
    touch_changed_overlays(output, specs, [path for path, _ in overlays])
    env = {
        'PATH': '/usr/bin:/bin:/usr/sbin:/sbin', 'LC_ALL': 'C', 'TZ': 'UTC',
        'ARCH': 'arm', 'LLVM': '1', 'CROSS_COMPILE': 'arm-linux-gnueabi-',
        'Y2_ARTIFACT_DIR': '/build', 'Y2_REBORN_SOURCE': '/tmp/Y2Reborn',
        'SOURCE_DATE_EPOCH': str(lock['source_date_epoch']),
        'KBUILD_BUILD_TIMESTAMP': '@' + str(lock['source_date_epoch']),
        'KBUILD_BUILD_USER': lock['build_user'], 'KBUILD_BUILD_HOST': lock['build_host'],
        'KBUILD_BUILD_VERSION': '1', 'PYTHONDONTWRITEBYTECODE': '1',
    }
    invocation = [
        'bwrap', '--unshare-all', '--uid', '0', '--gid', '0', '--die-with-parent',
        '--new-session', '--hostname', 'y2-build', '--clearenv',
        '--ro-bind', str(rootfs), '/', '--proc', '/proc', '--dev', '/dev',
        '--tmpfs', '/tmp', '--ro-bind', str(PROJECT), '/project',
        '--ro-bind', str(PROJECT / '.cache/sources/linux-6.18'), '/src',
        '--bind', str(output), '/build', '--chdir', '/build',
    ]
    # Reborn is needed only by the production builds; a kernel-only build (the Y1 first boot)
    # also runs on a machine without the sibling checkout.
    if REBORN.is_dir():
        invocation += ['--ro-bind', str(REBORN), '/tmp/Y2Reborn']
    # A private parent mount permits adding Y2 sources without creating files
    # in the locked upstream tree or replacing another platform's directory.
    invocation += ['--tmpfs', '/src/drivers']
    for child in sorted((PROJECT / '.cache/sources/linux-6.18/drivers').iterdir()):
        invocation += ['--ro-bind', str(child), '/src/drivers/' + child.name]
    invocation += ['--ro-bind', str(PROJECT / 'kernel/platform'), '/src/drivers/y2']
    invocation += ['--tmpfs', '/src/sound/soc/mediatek']
    for child in sorted((PROJECT / '.cache/sources/linux-6.18/sound/soc/mediatek').iterdir()):
        invocation += ['--ro-bind', str(child), '/src/sound/soc/mediatek/' + child.name]
    invocation += ['--ro-bind', str(PROJECT / 'kernel/audio'), '/src/sound/soc/mediatek/mt6582']
    # /src/drivers itself is already a private listing (above); its subdirectories are not.
    upstream = PROJECT / '.cache/sources/linux-6.18'
    invocation += expose_directories(upstream, [spec['path'] for spec in added
                                                if str(Path(spec['path']).parent) != 'drivers'])
    for spec in added:
        invocation += ['--ro-bind', str(PROJECT / spec['source']), '/src/' + spec['path']]
    for overlay, path in overlays:
        invocation += ['--ro-bind', str(overlay), '/src/' + path]
    for key, value in env.items():
        invocation += ['--setenv', key, value]
    raise SystemExit(subprocess.call(invocation + ['--'] + command))


if __name__ == '__main__':
    main()
