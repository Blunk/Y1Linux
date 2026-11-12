"""The Innioasis Y1's capabilities (docs/planning/Y1-RECOVERY-PLAN.md, R4b): the Y2ROOT build's
etc/y2linux/capabilities.json with over-the-air updates reported off. The Y1 runs the update service's daily
check and health acknowledgement, but an install through its recovery image is qualified only by R5's device run;
R6 drops this override. PID1 installs the result over the root's file (initramfs/y1-system.h).

    python3 -m tools.build.y1_capabilities TARGET OUT
"""
import argparse
import json
from pathlib import Path

from tools.validation.d08 import require

SOURCE = 'etc/y2linux/capabilities.json'
REASON = 'Innioasis Y1: installs through its recovery image await the device run (Y1-RECOVERY-PLAN R5)'
# What the Y1 reports off; nothing else of the root's file changes.
OFF = ('ota', 'automatic_bootimg_update')
# PID1 reads an override in one 64 KiB chunk (initramfs/y1-system.h install_file).
LIMIT = 65535


def derive(capabilities):
    """The Y1's copy: each OFF capability disabled with REASON, everything else as the root's."""
    features = capabilities.get('capabilities')
    require(isinstance(features, dict), 'capabilities object')
    result = json.loads(json.dumps(capabilities))
    for name in OFF:
        require(isinstance(features.get(name), dict) and features[name].get('implemented') is True,
                'implemented capability ' + name)
        result['capabilities'][name].update(enabled=False, reason=REASON)
    return result


def build(target, out):
    capabilities = json.loads((Path(target) / SOURCE).read_text())
    text = json.dumps(derive(capabilities), indent=2) + '\n'
    require(len(text.encode()) <= LIMIT, 'capabilities fit PID1\'s override chunk')
    Path(out).write_text(text)
    return text


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('target', help='the Y2ROOT build\'s Buildroot target')
    parser.add_argument('out', help='the Y1 capabilities file to write')
    args = parser.parse_args()
    build(args.target, args.out)


if __name__ == '__main__':
    main()
