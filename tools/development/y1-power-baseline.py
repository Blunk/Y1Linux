#!/usr/bin/env python3
"""Battery drain baseline for the Innioasis Y1 (docs/planning/Y1-POWER-PLAN.md).

Starts tools/development/y1-power-log.sh detached on the player over SSH, shows its state, and turns its TSV into
a drain report. The player's percentage is a voltage estimate and there is no validated current sensor, so the
report gives the voltage slope and the daemon's percentage slope; the charger's sense-resistor current
(sense_basis bsp-200mohm-unvalidated) is shown as recorded, not trusted.

  y1-power-baseline.py start <label> [--interval 60] [--hours 24]   # then pull the USB cable
  y1-power-baseline.py status
  y1-power-baseline.py report [<remote log name> | --file <local tsv>] [--save <dir>]

Scenarios worth one run each (several hours, nothing else touched): idle with the screen off; playback to wired
headphones with the screen off; the same with the screen on. Wi-Fi and Bluetooth state go into the label.
"""
import argparse
import os
from pathlib import Path
import statistics
import subprocess
import sys

HERE = Path(__file__).resolve().parent
LOGGER = HERE / 'y1-power-log.sh'
REMOTE_DIR = '/data/power-baseline'
REMOTE_LOGGER = REMOTE_DIR + '/y1-power-log.sh'
HOST = os.environ.get('Y1_HOST', '10.42.0.1')
KEY = os.environ.get('Y1_SSH_KEY', str(Path.home() / '.ssh/y2linux_ed25519'))
CPU_FIELDS = ['cpu_user', 'cpu_nice', 'cpu_system', 'cpu_idle', 'cpu_iowait', 'cpu_irq', 'cpu_softirq']


def ssh(command, stdin=None):
    # The player's host key is new on some boots (Y1-POWER-PLAN.md D1): no known_hosts pinning for this tool.
    args = ['ssh', '-i', KEY, '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=8',
            '-o', 'StrictHostKeyChecking=no', '-o', 'UserKnownHostsFile=/dev/null', '-o', 'LogLevel=ERROR',
            'root@' + HOST, command]
    result = subprocess.run(args, input=stdin, capture_output=True)
    if result.returncode:
        sys.exit(f'ssh {command!r} failed: {result.stderr.decode(errors="replace").strip()}')
    return result.stdout.decode(errors='replace')


def start(args):
    running = ssh(f'cat {REMOTE_DIR}/running.pid 2>/dev/null && kill -0 "$(cat {REMOTE_DIR}/running.pid)" '
                  '2>/dev/null && echo alive || true')
    if 'alive' in running:
        sys.exit('a logger is already running (status shows it)')
    # Below about 3.9 V the run lands in the cell's steep tail and hits the 3.50 V stop within the hour (the first
    # idle run, 2026-11-15: 3.63 V -> 3.50 V in 0.72 h), which says nothing about the mid-charge drain.
    uv = ssh('cat /sys/class/power_supply/BAT0/voltage_now').strip()
    if uv.isdigit() and int(uv) < 3_900_000 and not args.force:
        sys.exit(f'battery at {int(uv) / 1e6:.2f} V: charge to full first (or pass --force)')
    ssh(f'mkdir -p {REMOTE_DIR} && cat > {REMOTE_LOGGER}', stdin=LOGGER.read_bytes())
    ssh(f'setsid nohup sh {REMOTE_LOGGER} {args.label} {args.interval} {args.hours} '
        f'> {REMOTE_DIR}/last-start.err 2>&1 < /dev/null &')
    print(f'Logger started ({args.label}, every {args.interval} s, at most {args.hours} h).')
    print('Pull the USB cable within 10 minutes; plug it back in to end the run, then run "report".')


def status(_args):
    print(ssh(f'cd {REMOTE_DIR} 2>/dev/null || {{ echo "no runs yet"; exit 0; }}; '
              'if [ -f running.pid ] && kill -0 "$(cat running.pid)" 2>/dev/null; then '
              'echo "running (pid $(cat running.pid))"; else echo "not running"; fi; '
              'cat last-start.err 2>/dev/null; ls -1t *.tsv 2>/dev/null | head -n 5; '
              'latest=$(ls -1t *.tsv 2>/dev/null | head -n 1); '
              '[ -n "$latest" ] && { echo "-- $latest:"; tail -n 3 "$latest"; '
              'cat "$latest.done" 2>/dev/null | head -n 1; }'), end='')


def fetch(name):
    if not name:
        name = ssh(f'ls -1t {REMOTE_DIR}/*.tsv 2>/dev/null | head -n 1').strip()
        if not name:
            sys.exit('no log on the player')
    elif '/' not in name:
        name = f'{REMOTE_DIR}/{name}'
    return Path(name).name, ssh(f'cat {name}'), ssh(f'cat {name}.done 2>/dev/null || true')


def parse(text):
    header, rows, comments = None, [], []
    for line in text.splitlines():
        if line.startswith('#'):
            comments.append(line)
        elif header is None:
            header = line.split('\t')
        elif line:
            rows.append(dict(zip(header, line.split('\t'))))
    return comments, rows


def number(row, key):
    try:
        return float(row[key])
    except (KeyError, ValueError):
        return None


def slope_per_hour(rows, key):
    """Least-squares slope of a column against time, per hour; None with fewer than 3 usable points."""
    points = [(float(r['epoch']), v) for r in rows if (v := number(r, key)) is not None]
    if len(points) < 3:
        return None
    t0 = points[0][0]
    xs = [(t - t0) / 3600 for t, _ in points]
    ys = [v for _, v in points]
    mx, my = statistics.fmean(xs), statistics.fmean(ys)
    sxx = sum((x - mx) ** 2 for x in xs)
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx if sxx else None


def cpu_busy_percent(rows):
    first, last = rows[0], rows[-1]
    delta = {k: number(last, k) - number(first, k) for k in CPU_FIELDS}
    total = sum(delta.values())
    return 100 * (total - delta['cpu_idle'] - delta['cpu_iowait']) / total if total else None


def describe(name, text, done):
    comments, rows = parse(text)
    rows = [r for r in rows if r.get('usb') != '1']
    print(f'{name}')
    for line in comments:
        print(' ', line)
    if done:
        print('  end:', done.splitlines()[0])
    if len(rows) < 3:
        print('  fewer than 3 samples on battery: nothing to report')
        return
    hours = (float(rows[-1]['epoch']) - float(rows[0]['epoch'])) / 3600
    mv_h = slope_per_hour(rows, 'voltage_uv')
    pct_h = slope_per_hour(rows, 'daemon_percent')
    screen_on = sum(r.get('bl_power') == '0' for r in rows) / len(rows)
    sense = [v for r in rows if (v := number(r, 'sense_ua')) is not None]
    print(f'  on battery: {hours:.2f} h, {len(rows)} samples, screen on {100 * screen_on:.0f} % of samples')
    print(f'  voltage: {number(rows[0], "voltage_uv") / 1e6:.3f} V -> {number(rows[-1], "voltage_uv") / 1e6:.3f} V'
          + (f', slope {mv_h / 1000:+.1f} mV/h' if mv_h is not None else ''))
    if pct_h is not None:
        p0, p1 = number(rows[0], 'daemon_percent'), number(rows[-1], 'daemon_percent')
        print(f'  daemon percentage: {p0:.0f} -> {p1:.0f}, slope {pct_h:+.2f} %/h'
              + (f', about {100 / -pct_h:.0f} h from full to empty at this rate' if pct_h < 0 else ''))
    busy = cpu_busy_percent(rows)
    if busy is not None:
        print(f'  CPU busy: {busy:.1f} % (both cores, from /proc/stat)')
    freqs = sorted({r.get('cpu0_khz') for r in rows})
    print(f'  cpu0 frequency samples: {", ".join(freqs)}')
    if sense:
        print(f'  charger sense estimate (unvalidated): median {statistics.median(sense) / 1000:.1f} mA')
    for line in done.splitlines()[1:]:
        print('  ' + line)


def report(args):
    if args.file:
        name, text = args.file.name, args.file.read_text()
        done_path = args.file.with_name(args.file.name + '.done')
        done = done_path.read_text() if done_path.exists() else ''
    else:
        name, text, done = fetch(args.name)
        if args.save:
            args.save.mkdir(parents=True, exist_ok=True)
            (args.save / name).write_text(text)
            (args.save / (name + '.done')).write_text(done)
    describe(name, text, done)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('start')
    p.add_argument('label')
    p.add_argument('--interval', type=int, default=60)
    p.add_argument('--hours', type=int, default=24)
    p.add_argument('--force', action='store_true', help='start even below 3.9 V')
    p.set_defaults(run=start)
    sub.add_parser('status').set_defaults(run=status)
    p = sub.add_parser('report')
    p.add_argument('name', nargs='?')
    p.add_argument('--file', type=Path)
    p.add_argument('--save', type=Path)
    p.set_defaults(run=report)
    args = parser.parse_args()
    args.run(args)


if __name__ == '__main__':
    main()
