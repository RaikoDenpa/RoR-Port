"""Catalogue and GS feature census of a folder of PCSX2 GS dumps (the real game's: extracted/gsref/pcsx2_real/).

  python tools/port/gs_census.py DIR [--json OUT.json] [--index]

Per dump (.gs, .gs.xz, .gs.zst; parsed with gs_dump.py's model): the header check (serial SLES-54218, CRC 0x52585249),
vsyncs, transfers per path index, primitives, textures, image transfers and render-target-as-texture draws. Then the
census over all dumps: primitive types, texture formats and CLUT modes, filtering and wrap modes, blend/alpha/Z test
modes, frame/Z formats, the other per-pixel switches, each with its draw count and the number of dumps using it.
--index also writes DIR/index.md: thumbnail (DIR/thumbs/STEM.png), the description and suggested name from DIR/notes.tsv
(STEM<TAB>name<TAB>description, written by hand from the replays), the stats.

The real game's dumps hold PCSX2's GIF unit output: every packet as path index 3 (MTGS.cpp GS_RINGTYPE_GSPACKET ->
GSgifTransfer -> Transfer<3>), all paths merged, so they have no PATH1/2/3 split. Our dumps (src/ror_gsdump.cpp) keep
the paths apart (1 PATH2, 2 PATH3, 3 PATH1).
"""
import argparse, collections, glob, json, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gs_dump

SERIAL, CRC = 'SLES-54218', 0x52585249
PSM = {0x00: 'CT32', 0x01: 'CT24', 0x02: 'CT16', 0x0a: 'CT16S', 0x13: 'T8', 0x14: 'T4', 0x1b: 'T8H', 0x24: 'T4HL',
       0x2c: 'T4HH', 0x30: 'Z32', 0x31: 'Z24', 0x32: 'Z16', 0x3a: 'Z16S'}
TFX = ['MODULATE', 'DECAL', 'HIGHLIGHT', 'HIGHLIGHT2']
WRAP = ['REPEAT', 'CLAMP', 'REGION_CLAMP', 'REGION_REPEAT']
MMIN = ['NEAREST', 'LINEAR', 'NEAREST_MIPMAP_NEAREST', 'NEAREST_MIPMAP_LINEAR', 'LINEAR_MIPMAP_NEAREST',
        'LINEAR_MIPMAP_LINEAR', 'min6', 'min7']
ATST = ['NEVER', 'ALWAYS', 'LESS', 'LEQUAL', 'EQUAL', 'GEQUAL', 'GREATER', 'NOTEQUAL']
AFAIL = ['KEEP', 'FB_ONLY', 'ZB_ONLY', 'RGB_ONLY']
ZTST = ['NEVER', 'ALWAYS', 'GEQUAL', 'GREATER']
BLEND_ABD = ['Cs', 'Cd', '0', 'res']
BLEND_C = ['As', 'Ad', 'FIX', 'res']
TRXDIR = ['upload (host->GS)', 'readback (GS->host)', 'local->local copy', 'none']


def psm(v):
    return PSM.get(v, 'psm%02x' % v)


class Dump:
    """One dump through gs_dump.Gs with a kick hook that tallies the state of every primitive."""

    def __init__(self, path):
        self.path = path
        d = gs_dump.load(path)
        serial, crc, version, sp, ss, p = gs_dump.header(d)
        self.serial, self.crc, self.version = serial, crc, version
        self.ok = serial == SERIAL and crc == CRC
        self.features = collections.Counter()   # 'group: value' -> draws
        self.textures = set()                     # (TBP0, PSM) of textured draws
        self.rt = set()                            # FBP of every frame buffer drawn to
        self.rt_tex = collections.Counter()        # self / other RT / Z as texture
        self.trx = collections.Counter()
        self.transfers = collections.Counter()
        self.prims_per_vsync = []
        gs = gs_dump.Gs(d[sp:sp + ss])
        gs.kick_hook = self.kick
        n = 0
        while p < len(d):
            t = d[p]
            p += 1
            if t == 0:
                path, size = d[p], struct.unpack_from('<I', d, p + 1)[0]
                gs.transfer(path, d[p + 5:p + 5 + size])
                self.transfers[path] += 1
                p += 5 + size
            elif t == 1:
                p += 1
                self.vsync(gs, n)
                n = len(gs.draws)
            elif t == 2:
                p += 4
            elif t == 3:
                p += 8192
            elif t in (0x40, 0x41):
                p += 4 + struct.unpack_from('<I', d, p)[0]
            else:
                raise ValueError('bad packet type %d at %d' % (t, p - 1))
        self.vsync(gs, n)
        self.prims = sum(self.prims_per_vsync)

    def vsync(self, gs, n):
        if gs.draws or gs.writes:
            self.prims_per_vsync.append(len(gs.draws))
        for _, a, v in gs.writes:
            if a == 0x53:
                self.trx[TRXDIR[v & 3]] += 1
        gs.draws, gs.writes = [], []

    def kick(self, gs, path, ctxt):
        r, f = gs.reg, self.features
        t = gs.prim & 7
        attr = gs.prim if r.get(0x1a, 1) & 1 else r.get(0x1b, 0)
        tme, fge, abe, aa1, fst, iip = (attr >> 4) & 1, (attr >> 5) & 1, (attr >> 6) & 1, (attr >> 7) & 1, \
            (attr >> 8) & 1, (attr >> 3) & 1
        f['prim: ' + gs_dump.PRIMS[t]] += 1
        f['shading: ' + ('gouraud' if iip else 'flat')] += 1
        if fge:
            f['fog: on'] += 1
        if aa1:
            f['AA1: on'] += 1
        if ctxt:
            f['context: 2'] += 1
        frame, zbuf, test = r.get(0x4c + ctxt, 0), r.get(0x4e + ctxt, 0), r.get(0x47 + ctxt, 0)
        fbp, zbp = frame & 0x1ff, zbuf & 0x1ff
        self.rt.add(fbp)
        f['frame: %s FBW %d' % (psm((frame >> 24) & 0x3f), (frame >> 16) & 0x3f)] += 1
        if frame >> 32:
            f['frame: FBMSK %08x' % (frame >> 32)] += 1
        ate, zte = test & 1, (test >> 16) & 1
        if zte:
            f['Z: %s %s%s' % (psm(0x30 | ((zbuf >> 24) & 0xf)), ZTST[(test >> 17) & 3],
                              ' no write' if (zbuf >> 32) & 1 else '')] += 1
        else:
            f['Z: test off (ZTE 0)'] += 1
        if ate:
            f['alpha test: %s AREF %02x AFAIL %s' % (ATST[(test >> 1) & 7], (test >> 4) & 0xff,
                                                     AFAIL[(test >> 12) & 3])] += 1
        if (test >> 14) & 1:
            f['destination alpha test: DATM %d' % ((test >> 15) & 1)] += 1
        if abe:
            a = r.get(0x42 + ctxt, 0)
            fix = ' FIX %02x' % ((a >> 32) & 0xff) if (a >> 4) & 3 == 2 else ''
            f['blend: (%s - %s) * %s + %s%s' % (BLEND_ABD[a & 3], BLEND_ABD[(a >> 2) & 3], BLEND_C[(a >> 4) & 3],
                                                 BLEND_ABD[(a >> 6) & 3], fix)] += 1
            if r.get(0x49, 0) & 1:
                f['PABE: on'] += 1
        else:
            f['blend: off'] += 1
        if r.get(0x4a + ctxt, 0) & 1:
            f['FBA: on'] += 1
        if r.get(0x45, 0) & 1:
            f['dither: on'] += 1
        if not r.get(0x46, 1) & 1:
            f['COLCLAMP: wrap'] += 1
        if not tme:
            f['texture: none'] += 1
            return
        tex0, tex1, clamp = r.get(0x06 + ctxt, 0), r.get(0x14 + ctxt, 0), r.get(0x08 + ctxt, 0)
        tbp, tpsm = tex0 & 0x3fff, (tex0 >> 20) & 0x3f
        self.textures.add((tbp, tpsm))
        f['texture format: ' + psm(tpsm)] += 1
        f['texture function: %s TCC %d' % (TFX[(tex0 >> 35) & 3], (tex0 >> 34) & 1)] += 1
        f['texture coords: ' + ('UV' if fst else 'STQ')] += 1
        if tpsm in (0x13, 0x14, 0x1b, 0x24, 0x2c):
            f['CLUT: %s %s' % (psm((tex0 >> 51) & 0xf), 'CSM2' if (tex0 >> 55) & 1 else 'CSM1')] += 1
        if tpsm in (0x01, 0x02, 0x0a) or (tpsm in (0x13, 0x14, 0x1b, 0x24, 0x2c) and (tex0 >> 51) & 0xf):
            texa = r.get(0x3b, 0)
            f['TEXA: TA0 %02x TA1 %02x AEM %d' % (texa & 0xff, (texa >> 32) & 0xff, (texa >> 15) & 1)] += 1
        mxl = (tex1 >> 2) & 7
        f['filter: MAG %s MIN %s%s' % ('LINEAR' if (tex1 >> 5) & 1 else 'NEAREST', MMIN[(tex1 >> 6) & 7],
                                      ' MXL %d' % mxl if mxl else '')] += 1
        f['wrap: %s / %s' % (WRAP[clamp & 3], WRAP[(clamp >> 2) & 3])] += 1
        if tbp == fbp * 32:
            self.rt_tex['self (texture = its own render target)'] += 1
        elif tbp in {b * 32 for b in self.rt}:
            self.rt_tex['another render target'] += 1
        elif zte and tbp == zbp * 32:
            self.rt_tex['the Z buffer'] += 1

    def stats(self):
        return {'file': os.path.basename(self.path), 'ok': self.ok, 'serial': self.serial, 'crc': '%08x' % self.crc,
                'vsyncs': len(self.prims_per_vsync), 'prims': self.prims, 'prims_per_vsync': self.prims_per_vsync,
                'transfers': {str(k): v for k, v in sorted(self.transfers.items())}, 'textures': len(self.textures),
                'trx': dict(self.trx), 'rt_tex': dict(self.rt_tex), 'render_targets': sorted(self.rt),
                'features': dict(self.features)}


def stem(path):
    name = os.path.basename(path)
    for ext in ('.gs.zst', '.gs.xz', '.gs'):
        if name.endswith(ext):
            return name[:-len(ext)]
    return name


def write_index(folder, dumps):
    notes = {}
    tsv = os.path.join(folder, 'notes.tsv')
    if os.path.exists(tsv):
        for line in open(tsv, encoding='utf-8'):
            parts = line.rstrip('\n').split('\t')
            if len(parts) >= 3 and not line.startswith('#'):
                notes[parts[0]] = (parts[1], parts[2])
    rows = ['# Real-game GS dumps (PCSX2, SLES-54218)', '',
            'Dumped by the user from the PAL game in PCSX2 v2.8.2 (2026-10-27). Thumbnails: the last frame of a '
            'replay through the pinned PCSX2 GS (`pcsx2-gsrunner -surfaceless`, `replay/`). Stats from '
            '`tools/port/gs_census.py` (the dumps hold PCSX2\'s merged GIF output: no PATH1/2/3 split). '
            'Suggested names are not applied until the user confirms.', '',
            '| # | Dump | Picture | On screen | Suggested name | Stats |', '|---|---|---|---|---|---|']
    for i, dm in enumerate(dumps, 1):
        s, st = dm.stats(), stem(dm.path)
        name, desc = notes.get(st, ('', ''))
        thumb = 'thumbs/%s.png' % st
        pic = '![](%s)' % thumb.replace(' ', '%20') if os.path.exists(os.path.join(folder, thumb)) else 'no frame'
        tex = ', '.join('%s %d' % (k.split(' ')[0], v) for k, v in s['trx'].items())
        rtt = sum(s['rt_tex'].values())
        rows.append('| %d | `%s` | %s | %s | `%s` | %s; %d vsyncs, %d primitives, %d textures; %s%s |' % (
            i, st.replace('Rule of Rose_SLES-54218_', ''), pic, desc, name, 'header ok' if s['ok'] else
            '**header: %s %s**' % (s['serial'], s['crc']), s['vsyncs'], s['prims'], s['textures'],
            'image transfers: ' + tex if tex else 'no image transfers', '; RT as texture %d' % rtt if rtt else ''))
    open(os.path.join(folder, 'index.md'), 'w', encoding='utf-8').write('\n'.join(rows) + '\n')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('folder')
    ap.add_argument('--json', help='write every dump\'s stats and the census here')
    ap.add_argument('--index', action='store_true', help='write FOLDER/index.md')
    args = ap.parse_args()
    paths = sorted(p for ext in ('*.gs', '*.gs.xz', '*.gs.zst') for p in glob.glob(os.path.join(args.folder, ext)))
    dumps, census, used_by = [], collections.Counter(), collections.Counter()
    for p in paths:
        try:
            dm = Dump(p)
        except Exception as e:  # noqa: BLE001 - report and go on
            print('%s: UNREADABLE (%s)' % (os.path.basename(p), e))
            continue
        dumps.append(dm)
        s = dm.stats()
        print('%s: %s, %d vsyncs, %d primitives, %d textures, transfers %s, trx %s, RT as texture %s' % (
            stem(p), 'ok' if dm.ok else 'HEADER %s %s' % (s['serial'], s['crc']), s['vsyncs'], s['prims'],
            s['textures'], s['transfers'], s['trx'], s['rt_tex']))
        census.update(dm.features)
        used_by.update(k for k, v in dm.features.items() if v)
        census.update({'RT as texture: ' + k: v for k, v in dm.rt_tex.items()})
        used_by.update('RT as texture: ' + k for k in dm.rt_tex)
        census.update({'image transfer: ' + k: v for k, v in dm.trx.items()})
        used_by.update('image transfer: ' + k for k in dm.trx)
    print('\ncensus over %d dumps (draws, dumps):' % len(dumps))
    for key in sorted(census, key=lambda k: (k.split(':')[0], -census[k])):
        if census[key]:
            print('  %-70s %9d %4d' % (key, census[key], used_by[key]))
    if args.json:
        json.dump({'dumps': [d.stats() for d in dumps], 'census': census, 'used_by': used_by},
                  open(args.json, 'w'), indent=1)
    if args.index:
        write_index(args.folder, dumps)


if __name__ == '__main__':
    main()
