"""Repeatable play runs for milestone 6 (notes/port.md checklist): new game to player control on the road, then a pad
script, with the player's and Brown's positions read from GamMan every K frames.

  python tools/port/play.py --steps "2250+75:ly=30;2340+50:ly=00" --until 2450 [--every 10] [--shots 2300,2400]
                            [--route road|...|court | --save-route NAME [--keep-card NAME]] [--fast F] [--peek ADDR:N]
                            [--env NAME=VALUE]... [--no-base]

The base script (BASE) is the regression run's new game, the picture book (Right on each page, Circle on Exit) and
Cross through the captions; player control starts at frame 2174 (STEP_01_OUTBARN, renderer.md 6.12). --route adds a
known route after it (ROUTES). Pad items: button names as config/pad.ini, rx/ry/lx/ly=HH to hold a stick byte (left
stick: x = byte +4, y = byte +5, 0x00 = up/left, 0xff = down/right, dead zone 0x60-0xa0; bedpad 0x101f74/0x101ffc;
magnitude > 0.97 runs, 0x101ccc -> MagLifePC 0x11add4), or go=X/Z to steer to a world point (ror_pad.cpp). The run
fast-forwards (ROR_FAST_UNTIL) to just before the route's end: the same run frame for frame, only faster.
--save-route starts from a save instead (tools/port/routes/, save_route below): a copy of its card, no BASE.

Positions: GamMan::update 0x1c6580 copies the player's actor position to GamMan+0x22020 and Brown's to +0x22040
(GamMan 0x7ab1c0): x y z w floats. Printed per peek frame with the distance moved since the previous one.
"""
import argparse, math, os, re, shutil, struct, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RUN = os.path.join(ROOT, 'extracted', 'ps2recomp', 'run')
BASE = ';'.join(['100+3:cross', '200+3:start', '300+3:start', '400+3:cross', '460+3:cross',
                 '820+3:right', '900+3:right', '980+3:right', '1060+3:right', '1140+3:circle'] +
                ['%d+3:cross' % f for f in range(1550, 2151, 100)])
CONTROL = 2174
PLAYER, BROWN = 0x7cd1e0, 0x7cd200  # GamMan 0x7ab1c0 + 0x22020 / + 0x22040


def crosses(first, last, step):
    return ['%d+2:cross' % f for f in range(first, last + 1, step)]


def presses(*frames):
    return ['%d+2:cross' % f for f in frames]


# Routes after BASE (notes/port.md milestone 6), each with the frame where the player has control at its end:
#   road  player control on the road, A164 (2174)
#   shed  the road's gate and path (A164 -> A165 at 2598), the dog's bark (3311, captions), Strange Hill to the shed
#         (A167 at 3723), the shed door (3959, A166 at 4123), the post: Boarding Pass and Dog Collar (4500); control
#         inside the shed at 4815 (x -112, z -57)
ROUTES = {
    'road': ([], CONTROL),
    'shed': (['2250+80:lx=00', '2332+260:ly=00', '2600+430:go=400/2000', '3035+60:go=-900/2994',
              '3100+215:go=-900/500'] + crosses(3420, 3620, 40) +
             ['3625+275:go=-920/-912', '3905+40:go=-900/-700', '3946+8:go=-500/-700'] + crosses(3958, 4120, 30) +
             ['4445+45:go=-110/-60', '4492+5:go=-110/140', '4499+2:cross'] + crosses(4540, 4800, 25), 4815),
}
#   gate  from the shed: out of the door (A167 4971), north past the barn: the boy runs by (FLAG_01_RUN_JSH_BARN 5425),
#         back to A165 (5734), the sign's fork east and down (seek), A168 OG_GATE_F (7021): the captions at the
#         orphanage's gate, STEP_01_BACKGATE (7673); control at the gate at 7690 (x -109, z 2822)
ROUTES['gate'] = (ROUTES['shed'][0] + ['4820+50:go=-380/0'] + crosses(4872, 4960, 30) +
                  ['4980+150:go=-2600/-900', '5135+150:go=-1400/-2600', '5290+135:go=-1400/800'] +
                  crosses(5460, 5540, 40) +
                  ['5545+200:go=-1252/3000', '5750+60:go=-956/1300', '5815+120:go=-2500/1300', '5940+200:go=500/1300',
                   '6145+195:go=2500/-600', '6345+200:go=2000/-900', '6550+480:seek=0/-6000'] +
                  crosses(7140, 7660, 40), 7690)
#   backgate  from the gate: east along the wall to the fence area (A169 8129), the fence path, the pass door (the
#         use-item tutorial, the Boarding Pass used: FLAG_01_OPEN_BACKGATE 9691), the yard, the back-gate door entered
#         facing west (it needs heading 270° +- 90°, patch 0022): A170 OG_GATE_B (10039); control at 10060
ROUTES['backgate'] = (ROUTES['gate'][0] +
                      ['7700+300:seek=1900/2400', '8005+150:seek=2450/2000', '8160+150:seek=2250/1000',
                       '8315+120:seek=1950/700', '8440+150:seek=1950/100', '8595+150:seek=1950/-600',
                       '8750+150:seek=1950/-1300', '8905+150:seek=1700/-1950', '9060+40:go=1600/-1900',
                       '9105+45:go=1780/-1900', '9151+6:go=1695/-1900'] + crosses(9159, 9240, 40) +
                      ['9290+3:start', '9340+3:right', '9370+3:cross', '9400+3:down', '9430+3:cross'] +
                      crosses(9500, 9660, 40) +
                      ['9710+130:seek=1090/-1650', '9845+45:seek=1220/-1750', '9895+40:go=1065/-1750', '9940+2:cross'],
                      10060)
#   cway  from the rear yard (A170, its door to the hall is locked in chapter 1: world script 0xf08): back through
#         og_fence to A169 (10247), north up the western walkway (camera zones x 1807-2051) to door 3060, A168's grounds
#         (11222, FLAG_01_DOGCRY_OGGATEF), the front steps (the door regions are at their top, y -60), Cross: the
#         "ENTRANCE THIS WAY" notice (FLAG_01_LOOK_ENTRANCEPAPER), A171 O1_CWAY (12177), STEP_01_RUNBOY02 and its
#         event; control inside the front doors at 12831 (x -90, z 1050)
ROUTES['cway'] = (ROUTES['backgate'][0] +
                  ['10070+40:seek=880/-1750', '10115+30:go=1040/-1750'] + crosses(10148, 10230, 41) +
                  ['10290+80:seek=1500/-1500', '10375+80:seek=1760/-1420', '10460+80:seek=1920/-1250',
                   '10545+150:seek=1930/-600', '10700+150:seek=1930/100', '10855+150:seek=1930/700',
                   '11010+80:seek=1950/960', '11095+25:go=1950/1080'] + crosses(11123, 11285, 40) +
                  ['11300+105:seek=-90/1320', '11520+90:seek=1100/1500', '11640+180:seek=-90/1500',
                   '11825+80:seek=-90/1460', '11910+100:go=-90/1120'] + crosses(12015, 12815, 40), 12831)
#   knight  from the hall's entrance: into the hall (STEP_01_RUNBOY03 and its event), the o1_restway door (west wall),
#         A173 O1_RESTWAY (13699), the Bucket Knight (the rubbish bin, x -160 facing north): its first meeting and the
#         game's own Save menu (slot 0 saved, 14172), Circle back to play: control at 14335
ROUTES['knight'] = (ROUTES['cway'][0] +
                    ['12840+120:seek=-60/550', '12965+80:seek=-20/300'] + crosses(13050, 13450, 40) +
                    ['13470+80:seek=-120/300', '13555+40:go=-290/300'] + crosses(13598, 13680, 41) +
                    ['13780+60:go=-160/0', '13841+6:go=-160/400', '13849+2:cross'] + crosses(13890, 14130, 40) +
                    ['14230+3:circle', '14280+3:circle', '14330+3:circle'], 14335)
#   stepway  from the knight: back to the hall (A171 14546), its stairs from the south end (floor switch 14932), the
#         doll on the landing (y -300: STEP_01_LOOKDOLL 15202), the upper flight to the floor-2 gallery (y -400), the
#         o2_restway door (A191 15764), o2_blur (A193 "Filth Room" 15993: STEP_01_RUNBOY04 16135), back to the hall
#         (STEP_01_RUNBOY05 16715), the gallery's o2_way door (A199 16999: STEP_01_RUNBOY06 17136), along O2_WAY to the
#         o1_stepway door: A180 O1_STEPWAY (17813, FLAG_01_JSH_O1STEPWAY); control at 17830 (x 100, y -480, z 0)
ROUTES['stepway'] = (ROUTES['knight'][0] +
                     ['14345+60:seek=120/0', '14410+30:go=240/0'] + presses(14443, 14485, 14525) +
                     ['14600+140:seek=60/-640', '14745+80:seek=187/-640', '14830+80:seek=187/-420',
                      '14915+80:seek=300/-375', '15000+80:seek=300/-150', '15085+100:seek=280/100', '15190+2:cross',
                      '15230+40:go=290/120', '15275+60:go=200/120', '15340+60:go=110/120', '15405+40:go=40/130',
                      '15450+60:go=40/300', '15515+100:go=-150/300', '15620+40:go=-290/300'] +
                     presses(15663, 15705, 15745) + ['15800+50:go=0/30', '15855+30:go=0/-70'] +
                     presses(15888, *range(15930, 16171, 40)) + ['16210+30:go=0/110', '16245+40:go=0/250'] +
                     presses(16288, 16330, 16370) + ['16430+60:go=150/0', '16495+30:go=245/0'] +
                     presses(16528, 16570, 16610, 16660, 16700) +
                     ['16730+130:go=100/300', '16865+30:go=215/300'] + presses(16898, 16940, 16980) +
                     ['17060+150:go=450/550', '17220+80:seek=650/550', '17305+80:seek=800/400', '17390+120:seek=800/-200',
                      '17515+80:seek=750/-450', '17600+60:seek=650/-450', '17665+40:go=520/-450'] +
                     presses(17708, 17750, 17790), 17830)
#   court  from the stepway door: the floor-3 stairwell, A206 OR_MEG (STEP_01_JOSHUA), A204 OR_HALL (camera pan,
#         ORDER_READ_PICTBOOK), the storybook used from the item menu (Left, Cross, Down, Cross, Right on each page,
#         Circle: STEP_01_FUNERAL 20635 and the play-funeral cutaway), out through the attic hall door to A206
#         (STEP_01_DOGSTOPCRY 22297), down to floor 1 (23396), the o1_court door: A189 O1_COURT (STEP_01_DIGDUG 23799);
#         the shovel (object 0xa200001b, a189 f0x27a0: at x 150, z 70; Cross within 70, facing it): STEP_01_END and
#         ac_jump_world (24419, jump.sub10(2)), W002's chapter start (STEP_02_TITLE 24464) and its "Would you like to
#         save?" (24600; the card made at the chapter's end: --steps "24600+3:cross;24700+3:down;24760+3:cross", slot 2)
ROUTES['court'] = (ROUTES['stepway'][0] + [
                    '17840+60:go=90/-280', '17905+60:go=70/-440', '17980+60:go=80/-700', '18045+40:go=-60/-700',
                    '18090+90:go=-70/-250', '18185+50:go=-40/0', '18240+40:go=-125/0', '18283+2:cross',
                    '18325+2:cross', '18365+2:cross', '18410+80:seek=0/-600', '18495+50:go=0/-830',
                    '18560+80:seek=0/-1000', '18645+80:seek=-100/-1200', '18730+150:seek=-600/-1220',
                    '18885+40:go=-600/-1130', '18928+2:cross', '18970+2:cross', '19010+2:cross',
                    '19410+200:seek=-650/-300', '19615+80:go=-650/15', '19705+2:cross', '19745+2:cross',
                    '19785+2:cross', '19830+3:start', '19870+3:left', '19900+3:cross', '19930+3:down',
                    '19960+3:cross', '20060+3:right', '20120+3:right', '20180+3:right', '20240+3:right',
                    '20300+3:right', '20360+3:right', '20420+3:right', '20480+3:right', '20540+3:right',
                    '20600+3:circle', '20660+3:cross', '20710+3:cross', '20760+3:cross', '20810+3:cross',
                    '20860+3:cross', '20910+3:cross', '20960+3:cross', '21010+3:cross', '21060+3:cross',
                    '21110+3:cross', '21160+3:cross', '21210+3:cross', '21260+3:cross', '21310+3:cross',
                    '21360+3:cross', '21410+3:cross', '21460+3:cross', '21510+3:cross', '21560+3:cross',
                    '21610+3:cross', '21660+3:cross', '21710+3:cross', '21760+3:cross', '21810+3:cross',
                    '21860+3:cross', '21910+3:cross', '21950+150:seek=-600/-900', '22105+50:go=-600/-1070',
                    '22158+2:cross', '22200+2:cross', '22240+2:cross', '22280+2:cross', '22320+2:cross',
                    '22340+30:go=-580/-1210', '22375+60:go=-206/-1205', '22440+30:go=-97/-1190',
                    '22475+40:go=0/-996', '22520+50:go=0/-700', '22575+60:go=300/-550', '22640+30:go=395/-550',
                    '22673+2:cross', '22715+2:cross', '22755+2:cross', '22840+40:go=-40/0', '22885+90:go=-70/-250',
                    '22980+90:go=-60/-700', '23075+40:go=80/-700', '23120+60:go=70/-440', '23185+60:go=90/-200',
                    '23250+40:go=-70/-240', '23295+80:go=-70/-680', '23380+40:go=60/-700', '23425+80:go=80/-450',
                    '23510+60:go=80/-100', '23575+80:go=-250/-60', '23660+30:go=-250/70', '23693+2:cross',
                    '23735+2:cross', '23775+2:cross', '24080+60:go=0/-100', '24145+60:go=150/0',
                    '24210+30:go=150/60', '24245+2:cross', '24265+2:cross', '24285+2:cross', '24305+2:cross',
                    '24325+2:cross', '24345+2:cross', '24365+2:cross', '24385+2:cross', '24405+2:cross'], 24600)

# Routes from the port's own saves (tools/port/routes/NAME.txt: the full pad script, title to the slot's Load Game
# included; its header names the card it loads, under run/, and the card the run saves into). Each card is made by the
# route before it (--keep-card): court -> card_ch2 -> ch2_scarecrow -> card_ch2b -> ch2_butterfly -> card_ch2c ->
# ch2_scissors -> card_ch2d -> ch2_getdog (ch2_onekey: the same and on) -> card_ch2e -> ch2_upper (ch2_bluebtf: the
# same and on) -> card_ch2f -> ch2_gift (chapter 2's end) -> card_ch3 -> ch3_start.
ROUTE_DIR = os.path.join(ROOT, 'tools', 'port', 'routes')


def save_route(name):
    """(card folder under run/, pad script, frame of player control at its end or 0)"""
    lines = open(os.path.join(ROUTE_DIR, name + '.txt'), encoding='utf-8').read().splitlines()
    head = ' '.join(l for l in lines if l.startswith('#'))
    card = re.search(r'card: (\w+)', head).group(1)
    control = re.search(r'control at (\d+)', head)
    return card, ';'.join(l for l in lines if l and not l.startswith('#')), int(control.group(1)) if control else 0


def floats(words):
    return [struct.unpack('<f', struct.pack('<I', int(w, 16)))[0] for w in words]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--steps', default='', help='pad script steps after the base script')
    ap.add_argument('--until', type=int, required=True, help='last frame')
    ap.add_argument('--every', type=int, default=10, help='position peek interval (frames)')
    ap.add_argument('--from', dest='start', type=int, default=CONTROL, help='first peek frame')
    ap.add_argument('--shots', default='', help='ROR_SCREENSHOT frames')
    ap.add_argument('--env', action='append', default=[])
    ap.add_argument('--no-base', action='store_true', help='only --steps (e.g. after loading a save)')
    ap.add_argument('--seconds', type=int, default=0)
    ap.add_argument('--route', choices=sorted(ROUTES), default='road', help='steps before --steps (ROUTES)')
    ap.add_argument('--save-route', default=None, help='a route from a save (tools/port/routes/NAME.txt): its card '
                    '(copied, the original stays as it is), no BASE, its steps before --steps')
    ap.add_argument('--keep-card', default=None, help="after the run, copy the run's card to run/NAME (for the next "
                    "save route)")
    ap.add_argument('--card', default=None, help='memory card folder (ROR_MC_DIR) kept between runs; default: '
                    'an empty card per run (run/memcard_play), as BASE expects')
    ap.add_argument('--fast', type=int, default=None,
                    help='ROR_FAST_UNTIL: 5 ms VBlanks until this frame (default: the route\'s end - 24; 0 = real time)')
    ap.add_argument('--peek', action='append', default=[], help='ADDR:N (hex, words): also peeked at each peek frame, '
                    'printed raw (MagLifePC 0x750a60: +0 mode, +0xe0 walk 1 / run 2, +0x104 stick magnitude)')
    args = ap.parse_args()
    route, control = ROUTES[args.route]
    card = os.path.abspath(args.card) if args.card else os.path.join(RUN, 'memcard_play')
    if not args.card:
        shutil.rmtree(card, ignore_errors=True)
    lead = [] if args.no_base else [BASE] + route
    fast = args.fast
    if args.save_route:
        if args.card:
            sys.exit('--save-route copies its own card; leave out --card')
        source, steps, control = save_route(args.save_route)
        shutil.copytree(os.path.join(RUN, source), card)
        lead = [steps]
    elif args.no_base and fast is None:
        fast = 0  # loading a save: no route end to fast-forward to unless --fast says so
    if fast is None:
        fast = control - 24
    script = ';'.join(s for s in lead + [args.steps] if s)
    peeks = ','.join(','.join(['%d:%x:12' % (f, PLAYER)] + ['%d:%s' % (f, x) for x in args.peek])
                     for f in range(args.start, args.until + 1, args.every))
    # the routes' frames assume movies skipped and the sound placeholders (ROR_MOVIES=0, ROR_SOUND=0; --env overrides)
    env = ['ROR_PEEK=' + peeks] + (['ROR_SCREENSHOT=' + args.shots] if args.shots else []) + \
          (['ROR_FAST_UNTIL=%d' % fast] if fast > 0 else []) + ['ROR_MC_DIR=' + card, 'ROR_MOVIES=0', 'ROR_SOUND=0'] + args.env
    cmd = [sys.executable, os.path.join(ROOT, 'tools', 'port', 'build.py'), 'run', '--frames', str(args.until + 2),
           '--seconds', str(args.seconds or args.until // 20 + 120), '--pad-script', script]
    for e in env:
        cmd += ['--env', e]
    subprocess.run(cmd, check=True)
    if args.keep_card:
        kept = os.path.join(RUN, args.keep_card)
        shutil.rmtree(kept, ignore_errors=True)
        shutil.copytree(card, kept)
    rows, prev = {}, None
    for line in open(os.path.join(RUN, 'ror.log'), encoding='utf-8', errors='replace'):
        m = re.match(r'\[f(\d+)\] peek ([0-9a-f]{8}):((?: [0-9a-f]{8})+)', line)
        if m:
            rows.setdefault(int(m.group(1)), {})[int(m.group(2), 16)] = m.group(3).split()
    print('frame   player x       y       z     moved | Brown x       y       z')
    for f in sorted(rows):
        p, b = floats(rows[f].get(PLAYER, ['0'] * 4)), floats(rows[f].get(BROWN, ['0'] * 4))
        moved = math.dist(p[:3], prev[:3]) if prev else 0.0
        prev = p
        print('%5d %8.2f %8.2f %8.2f %7.2f | %8.2f %8.2f %8.2f' % (f, p[0], p[1], p[2], moved, b[0], b[1], b[2]))
        for x in args.peek:
            a, n = int(x.split(':')[0], 16), int(x.split(':')[1])
            print('      %08x:' % a, ' '.join(' '.join(rows[f].get(a + i * 4, [])) for i in range(0, n, 4)))


if __name__ == '__main__':
    main()
