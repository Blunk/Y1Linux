#!/usr/bin/env python3
"""Owner-local: the Innioasis Y1's own Bluetooth record (32-byte BT_Addr) from the owner's eMMC backup.

docs/planning/Y1-BLUETOOTH-PLAN.md, B6. Stock Android kept /nvram/APCFG/APRDEB/BT_Addr on USRDATA, which is
Y2DATA now: the record survives only in the owner's backup (a full logical eMMC dump). This reads it with
debugfs straight from the dump at USRDATA's offset (read only, no copy of the partition), checks it as the
driver does (32 bytes, unicast, non-zero, not the MT6572's generic default) and writes it 0600 for the Y1's
/data/connectivity/bt-record. It never prints the address (only its last two bytes) and never writes the backup.

usage: y1_bt_record.py --backup emmc_user.bin --output DIR
"""
import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile

# USRDATA in the logical dump (printgpt offset; the ext4 superblock magic was checked there, B6).
USRDATA_OFFSET = 0x4d540000
RECORD_PATH = '/nvram/APCFG/APRDEB/BT_Addr'
RECORD_SIZE = 32
# SHA-256 of 00:00:46:65:72:01, stock libbluetooth_mtk's generic default (kernel/y1/connectivity/y1-conn-policy.h).
GENERIC_DEFAULT_SHA256 = 'd3097b82864e724db5a8f3a0d21a5ded90a9173d2c04568fb18cbd76e27c1b85'


def reason(record):
    """None for a record the driver accepts, otherwise why not (the same rule as y1_conn_record_check)."""
    if len(record) != RECORD_SIZE:
        return 'record size'
    address = record[:6]
    if address[0] & 1:
        return 'multicast address'
    if not any(address):
        return 'zero address'
    if hashlib.sha256(address).hexdigest() == GENERIC_DEFAULT_SHA256:
        return 'the generic MT6572 default address'
    return None


def redacted(record):
    return 'xx:xx:xx:xx:%02x:%02x' % (record[4], record[5])


def extract(backup, output, offset=USRDATA_OFFSET):
    """Read the record from the dump, check it and write output/bt-record (0600). Returns the record."""
    output.mkdir(mode=0o700, parents=True, exist_ok=True)
    previous = os.umask(0o077)
    try:
        with tempfile.TemporaryDirectory(dir=output) as scratch:
            raw = Path(scratch) / 'BT_Addr'
            subprocess.run(['debugfs', '-R', f'dump {RECORD_PATH} {raw}', f'{backup}?offset={offset}'],
                           check=True, capture_output=True)
            if not raw.is_file():
                raise ValueError('no BT_Addr record in the backup')
            record = raw.read_bytes()
            why = reason(record)
            if why:
                raise ValueError('record refused: ' + why)
            target = output / 'bt-record'
            target.write_bytes(record)
            target.chmod(0o600)
    finally:
        os.umask(previous)
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--backup', type=Path, required=True, help="the owner's full logical eMMC dump")
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    record = extract(args.backup, args.output)
    # As y2-factory: never the address or a device-specific checksum, only its last two bytes.
    print(f'bt-record: 32 bytes, address {redacted(record)}, written 0600 to {args.output / "bt-record"}')


if __name__ == '__main__':
    main()
