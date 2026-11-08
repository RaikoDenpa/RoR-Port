"""Gameplay regression check (milestone 6, notes/port.md): one run along play.py's `backgate` route, against references.

  python tools/port/play_regress.py            run, compare with the references
  python tools/port/play_regress.py --update   run, then make this run the references (review it first)
  python tools/port/play_regress.py --compare  only compare the last run's outputs (no game run)

The run: new game (play.py BASE), the `backgate` route (the road, the dog's bark, the shed and its items, out past the
barn, Strange Hill, the orphanage's front gate, the fence area, the pass door opened with the Boarding Pass, the
back-gate door, which needs the facing check of patch 0022), then the item menu opened and closed in the rear yard. Fast-forwarded
(ROR_FAST_UNTIL; the run is the same frame for frame), PCSX2 GS with its default settings (ROR_GS_INI=none), an empty
memory card. Three checks against extracted/gsref/play/ (gitignored: screenshots are game pictures):

  events     the script's labels with their frames (area loads, flags, steps, playable/unplayable, doors, inserts),
             from ror.log: the same sequence at the same frames
  positions  the player's position (GamMan+0x22020) every 50 frames from player control: within 1 unit
  pictures   ROR_SCREENSHOT at the CASES frames: gs_regress.py's rule (at most 0.5 % of pixels off by more than 24)

For the CASES with a real-game dump (extracted/gsref/pcsx2_real, index.md), the best-matching frame of that dump is
also reported, for information (not part of pass/fail).
"""
import argparse, glob, os, re, shutil, struct, subprocess, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gs_regress  # noqa: E402
import play  # noqa: E402

ROOT = play.ROOT
RUN = play.RUN
REFDIR = os.path.join(ROOT, 'extracted', 'gsref', 'play')
REAL = os.path.join(ROOT, 'extracted', 'gsref', 'pcsx2_real')
ROUTE, CONTROL = play.ROUTES['backgate']
EXTRA = ['10080+3:start', '10170+3:circle']           # the item menu in the rear yard
LAST = 10240
POS_FROM, POS_EVERY = play.CONTROL, 50
# (frame, what, real-game dump number or None)
CASES = [
    (2200, 'road, player control', None),
    (3405, 'the dog\'s bark, first caption', 50),
    (3530, 'the dog\'s bark, second caption', 51),
    (3995, 'shed door: "...However, it\'s not locked."', 60),
    (4115, '"Rickety Shed" title', 62),
    (4290, 'shed interior', 67),
    (4541, 'post: "A slip of paper is tied to the dog collar."', 73),
    (4649, 'item board: Boarding Pass', 75),
    (4733, 'item board: Dog Collar', 77),
    (4815, 'shed, control after the items', None),
    (7320, 'gate caption "...Following the boy up the road,"', None),
    (7640, 'gate caption "For some reason, ..."', None),
    (8400, 'fence path by the mansion wall', None),
    (9480, 'the pass door, Boarding Pass used', None),
    (9960, 'the back-gate door opening', None),
    (10060, '"Rear Gate" title', None),
    (10140, 'item menu in the rear yard', None),
    (10230, 'back in play in the rear yard', None),
]
LABEL = re.compile(r'^\[f(\d+)\] script (\S+): ((?:\[(?:Area init|Set Flag|Set Step|playable|unplayable)\]|>Door|>Insert)'
                   r'[\x20-\x7e]*)$')


def floats(words):
    return [struct.unpack('<f', struct.pack('<I', int(w, 16)))[0] for w in words]


def run_game():
    card = os.path.join(RUN, 'memcard_regress')
    shutil.rmtree(card, ignore_errors=True)
    script = ';'.join([play.BASE] + ROUTE + EXTRA)
    peeks = ','.join('%d:%x:3' % (f, play.PLAYER) for f in range(POS_FROM, LAST + 1, POS_EVERY))
    env = ['ROR_FAST_UNTIL=%d' % (LAST - 10), 'ROR_GS_INI=none', 'ROR_MC_DIR=' + card, 'ROR_PEEK=' + peeks,
           'ROR_MOVIES=0',   # the route's frames assume movies skipped
           'ROR_SOUND=0',    # and the sound placeholders (every stream finished at once)
           'ROR_SCREENSHOT=' + ','.join(str(f) for f, _, _ in CASES)]
    cmd = [sys.executable, os.path.join(ROOT, 'tools', 'port', 'build.py'), 'run', '--frames', str(LAST + 2),
           '--seconds', '1200', '--pad-script', script]
    for e in env:
        cmd += ['--env', e]
    subprocess.run(cmd, check=True)


def read_run():
    """-> (events [(frame, context, label)], positions {frame: (x, y, z)}) from the last run's ror.log."""
    events, pos = [], {}
    for line in open(os.path.join(RUN, 'ror.log'), encoding='utf-8', errors='replace'):
        line = line.rstrip('\n')
        m = LABEL.match(line)
        if m:
            events.append((int(m.group(1)), m.group(2), m.group(3).rstrip()))
            continue
        m = re.match(r'^\[f(\d+)\] peek %08x: (\w+) (\w+) (\w+)' % play.PLAYER, line)
        if m:
            pos[int(m.group(1))] = tuple(floats(m.group(2, 3, 4)))
    return events, pos


def write_refs(events, pos):
    os.makedirs(REFDIR, exist_ok=True)
    with open(os.path.join(REFDIR, 'events.tsv'), 'w', encoding='utf-8', newline='\n') as f:
        for e in events:
            f.write('%d\t%s\t%s\n' % e)
    with open(os.path.join(REFDIR, 'positions.tsv'), 'w', encoding='utf-8', newline='\n') as f:
        for fr in sorted(pos):
            f.write('%d\t%.3f\t%.3f\t%.3f\n' % ((fr,) + pos[fr]))
    for fr, _, _ in CASES:
        shutil.copyfile(os.path.join(RUN, 'screen_%d.png' % fr), os.path.join(REFDIR, '%d.png' % fr))


def read_refs():
    events = [(int(a), b, c) for a, b, c in (l.rstrip('\n').split('\t', 2)
                                             for l in open(os.path.join(REFDIR, 'events.tsv'), encoding='utf-8'))]
    pos = {int(p[0]): tuple(float(x) for x in p[1:]) for p in (l.split()
                                                              for l in open(os.path.join(REFDIR, 'positions.tsv')))}
    return events, pos


def real_dump(number):
    """Folder of real dump `number` (index.md rows: | n | `stamp` | ...)."""
    for line in open(os.path.join(REAL, 'index.md'), encoding='utf-8'):
        parts = [p.strip() for p in line.split('|')]
        if len(parts) > 3 and parts[1] == str(number) and parts[2].startswith('`'):
            stamp = parts[2].strip('`')
            return os.path.join(REAL, 'replay', 'Rule of Rose_SLES-54218_%s' % stamp)
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--update', action='store_true')
    ap.add_argument('--compare', action='store_true', help='compare the last run only')
    args = ap.parse_args()
    if not args.compare:
        run_game()
    events, pos = read_run()
    missing = [fr for fr, _, _ in CASES if not os.path.exists(os.path.join(RUN, 'screen_%d.png' % fr))]
    if missing:
        sys.exit('no screenshot for frames %s (the run ended early?)' % missing)
    if args.update:
        write_refs(events, pos)
        print('references written to %s: %d events, %d positions, %d pictures' % (REFDIR, len(events), len(pos),
                                                                                    len(CASES)))
        return
    ref_events, ref_pos = read_refs()
    failures = 0
    # events
    if events == ref_events:
        print('events     ok  %d labels, the same frames' % len(events))
    else:
        failures += 1
        print('events     DIFFER (%d now, %d reference); first differences:' % (len(events), len(ref_events)))
        shown = 0
        for i in range(max(len(events), len(ref_events))):
            a = events[i] if i < len(events) else None
            b = ref_events[i] if i < len(ref_events) else None
            if a != b:
                print('  #%d now %s | ref %s' % (i, a, b))
                shown += 1
                if shown == 8:
                    break
    # positions
    worst = max(((max(abs(p - q) for p, q in zip(pos[f], ref_pos[f])) if f in pos else 1e9), f) for f in ref_pos)
    if worst[0] <= 1.0:
        print('positions  ok  %d frames, largest difference %.2f (frame %d)' % (len(ref_pos), worst[0], worst[1]))
    else:
        failures += 1
        f = worst[1]
        print('positions  DIFFER  frame %d: now %s, reference %s' % (f, pos.get(f), ref_pos[f]))
    # pictures
    for fr, what, real in CASES:
        ours = os.path.join(RUN, 'screen_%d.png' % fr)
        ok, text = gs_regress.compare(ours, os.path.join(REFDIR, '%d.png' % fr))
        failures += not ok
        line = '%-10s %-4s %s  [%s]' % (fr, 'ok' if ok else 'DIFF', text, what)
        folder = real_dump(real) if real else None
        if folder and os.path.isdir(folder):
            best = min((gs_regress.compare(ours, p)[1] for p in sorted(glob.glob(os.path.join(folder, '*.png')))),
                       key=lambda t: float(t.split(' ')[0]) if t[0].isdigit() else 1e9)
            line += '; real dump %d: %s' % (real, best)
        print(line)
    print('all match' if not failures else '%d check(s) differ' % failures)
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
