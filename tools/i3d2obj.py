"""I3D models (I3D_BIN, the game's "i3d" library) -> Wavefront OBJ + MTL + PNG textures, in the bind pose.

Usage:
  python i3d2obj.py <model.mdl RTPK | I3D_BIN file> --dump            tag tree summary + parts
  python i3d2obj.py <model.mdl RTPK | I3D_BIN file> --obj <out.obj>   writes out.obj, out.mtl and textures next to it
      [--textures <RTPK of TIM2s>]  only needed when giving a bare I3D_BIN; a .mdl RTPK carries its own
      [--model-index N]             which I3D_BIN of the .mdl (characters carry a low-poly second model)

The model data is Z-up (the frame before ROOTNODE's axis swap); the OBJ is written Y-up as (x, z, -y).

Format notes: notes/formats/i3d.md. In short:
  - tags are 16 bytes {u32 data, u32 count | type << 24 | nofix << 31, u32 children, u32 name}, offsets from +0x10
  - a geometry part is a type-0x56 tag; children: [0x02 strip groups, 0x03, 0x03>0x33 normals, 0x37 OBB,
    0x03>0x33 positions, 0x02 > 0x31 skin weights]
  - strip groups: 0x02 > 0x30 (one per material pass) > 0x02 > 0x47 strips; a strip = {u32 0x10, u8 qwc, u8 count,
    ...} + count * {u16 index | ADC << 15, u16 winding << 15}; its children 0x03>0x33 are per-strip-vertex
    attributes (the V2-32 one is the UVs)
  - 0x33 blocks = {u32 header size, u16 qwc, u16 vertex count, u32 kind, u32}: kind 1 = a VIF stream (UNPACK/STMASK/
    STROW/STCOL/STCYCL), decoded here by a small VIF model into "VU memory" vectors; kind 0 = raw float qwords
  - textures bind through materials: 0x4d u16 material -> 0x25 material -> 0x26 layer texture name
  - parts are placed by the skeleton: rigid meshes on the node owning their 0x59 instance, single-bone parts on
    their bone, skinned parts by blending v * inverse_bind[slot] * bone with the 0x31 weights
"""
import argparse
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rtpk  # noqa: E402
import tim2png  # noqa: E402

B = 0x10  # tag offsets are relative to file offset 0x10


class Tag:
    __slots__ = ('at', 'data', 'count', 'type', 'nofix', 'children', 'name')

    def __init__(self, d, at):
        data, w1, ch, nm = struct.unpack_from('<4I', d, at)
        self.at, self.count, self.type, self.nofix = at, w1 & 0xffffff, (w1 >> 24) & 0x7f, w1 >> 31
        self.data = B + data if data else None
        self.children = B + ch if ch else None
        self.name = d[B + nm:d.index(b'\0', B + nm)].decode('ascii', 'replace') if nm else ''


def kids(d, t):
    return [Tag(d, t.children + 16 * i) for i in range(t.count)] if t.children else []


def walk(d, t, depth=0):
    yield t, depth
    for k in kids(d, t):
        yield from walk(d, k, depth + 1)


# ---------------------------------------------------------------- VIF
FIELD_SIZES = {0: 4, 1: 2, 2: 1}


def vif_block(d, off):
    """Run the VIF stream of a type-0x33 block; return {qword address: [x, y, z, w]} (floats or ints)."""
    hdr, qwc, _nv = struct.unpack_from('<IHH', d, off)
    p, end = off + hdr, off + hdr + qwc * 16
    mem = {}
    cl = wl = 1
    mask = 0
    row = [0.0] * 4
    col = [0.0] * 4
    while p < end:
        code = struct.unpack_from('<I', d, p)[0]
        cmd, num, imm = code >> 24, (code >> 16) & 0xff, code & 0xffff
        p += 4
        if cmd & 0x60 == 0x60:
            vn, vl, m = (cmd >> 2) & 3, cmd & 3, (cmd >> 4) & 1
            n = num or 256
            comps = vn + 1
            if vl == 3:
                esz = 2
            else:
                esz = FIELD_SIZES[vl] * comps
            addr = imm & 0x3ff
            usn = (imm >> 14) & 1
            reads = n if cl >= wl else (n // wl) * cl + min(n % wl, cl)
            src = d[p:p + reads * esz]
            p += (reads * esz + 3) // 4 * 4
            r = 0
            for i in range(n):
                cyc = i % wl if wl else 0
                filling = cl < wl and cyc >= cl
                vals = [0.0] * 4
                if not filling:
                    e = src[r * esz:(r + 1) * esz]
                    r += 1
                    if vl == 0:
                        f = list(struct.unpack('<%df' % comps, e))
                    elif vl == 1:
                        f = list(struct.unpack('<%d%s' % (comps, 'H' if usn else 'h'), e))
                    elif vl == 2:
                        f = list(struct.unpack('<%d%s' % (comps, 'B' if usn else 'b'), e))
                    else:
                        c = struct.unpack('<H', e)[0]
                        f = [(c & 31) << 3, ((c >> 5) & 31) << 3, ((c >> 10) & 31) << 3, (c >> 15) << 7]
                    # V2/V3 leave the remaining fields "undefined"; V1 broadcasts. Fill like the VIF does for V1.
                    if comps == 1:
                        f = f * 4
                    vals[:len(f)] = f
                q = addr + i
                cur = mem.get(q, [0.0] * 4)
                mrow = min(cyc, 3)
                for k in range(4):
                    mode = (mask >> (mrow * 8 + k * 2)) & 3 if m or filling else 0
                    if filling and not m:
                        mode = 1
                    if mode == 0:
                        if not filling and k < max(len(vals), 4):
                            cur[k] = vals[k]
                    elif mode == 1:
                        cur[k] = row[k]
                    elif mode == 2:
                        cur[k] = col[mrow]
                mem[q] = cur
        elif cmd == 0x20:
            mask = struct.unpack_from('<I', d, p)[0]
            p += 4
        elif cmd == 0x01:
            cl, wl = imm & 0xff, (imm >> 8) & 0xff
        elif cmd == 0x30:
            row = list(struct.unpack_from('<4f', d, p))
            p += 16
        elif cmd == 0x31:
            col = list(struct.unpack_from('<4f', d, p))
            p += 16
        elif cmd in (0x00, 0x05, 0x10, 0x11, 0x13, 0x14, 0x15, 0x17):
            pass
        else:
            raise ValueError('unhandled VIF code %#010x at %#x' % (code, p - 4))
    return mem


def block_vectors(d, tag33, count=None):
    """Vertex vectors of a 0x33 block. Header word 3 (w2) = 1: VIF stream; 0: raw, one float qword per vertex."""
    hdr, qwc, nv, w2 = struct.unpack_from('<IHHI', d, tag33.data)
    n = count if count is not None else nv
    if w2 == 0:
        return [list(struct.unpack_from('<4f', d, tag33.data + hdr + 16 * i)) for i in range(min(n, qwc))]
    mem = vif_block(d, tag33.data)
    return [mem.get(i, [0.0] * 4) for i in range(n)]


def first33(d, t):
    """0x03 > 0x33 child -> the 0x33 tag (or None)."""
    for k in kids(d, t):
        if k.type == 0x33 and k.data is not None:
            return k
    return None


def unpack_formats(d, off):
    hdr, qwc, nv, w2 = struct.unpack_from('<IHHI', d, off)
    if w2 == 0:
        return [(4, 0, nv)]  # raw qwords
    hdr, qwc = struct.unpack_from('<IH', d, off)
    p, end, out = off + hdr, off + hdr + qwc * 16, []
    cl = wl = 1
    while p < end:
        code = struct.unpack_from('<I', d, p)[0]
        cmd, num = code >> 24, (code >> 16) & 0xff
        p += 4
        if cmd & 0x60 == 0x60:
            vn, vl = (cmd >> 2) & 3, cmd & 3
            n = num or 256
            esz = 2 if vl == 3 else FIELD_SIZES[vl] * (vn + 1)
            reads = n if cl >= wl else (n // wl) * cl + min(n % wl, cl)
            out.append((vn + 1, vl, n))
            p += (reads * esz + 3) // 4 * 4
        elif cmd == 0x20:
            p += 4
        elif cmd == 0x01:
            cl, wl = code & 0xff, (code >> 8) & 0xff
        elif cmd in (0x30, 0x31):
            p += 16
    return out


# ---------------------------------------------------------------- model
class Part:
    def __init__(self):
        self.positions = []
        self.normals = []
        self.strips = []   # list of (entries, uvs, pass_index, material_data)
        self.material = None  # u16 at +0xc of the nearest type-0x4d ancestor (material index)


def strip_entries(d, off):
    count = d[off + 5]
    hdr = struct.unpack_from('<I', d, off)[0]
    return [struct.unpack_from('<HH', d, off + hdr + 4 * i) for i in range(count)]


def walk_mat(d, t, mat=None):
    """Like walk(), also passing down the material index of the nearest type-0x4d ancestor."""
    if t.type == 0x4d and t.data is not None:
        mat = struct.unpack_from('<H', d, t.data + 0xc)[0]
    yield t, mat
    for k in kids(d, t):
        yield from walk_mat(d, k, mat)


# ---------------------------------------------------------------- skeleton
def mat_mul(a, b):
    """Row-vector convention (v' = v * M): returns a * b."""
    return [sum(a[r * 4 + k] * b[k * 4 + c] for k in range(4)) for r in range(4) for c in range(4)]


def mat_inv(m):
    a = [list(m[i * 4:i * 4 + 4]) for i in range(4)]
    inv = [[float(i == j) for j in range(4)] for i in range(4)]
    for c in range(4):
        p = max(range(c, 4), key=lambda r: abs(a[r][c]))
        a[c], a[p] = a[p], a[c]
        inv[c], inv[p] = inv[p], inv[c]
        pv = a[c][c]
        a[c] = [x / pv for x in a[c]]
        inv[c] = [x / pv for x in inv[c]]
        for r in range(4):
            if r != c:
                fc = a[r][c]
                a[r] = [x - fc * y for x, y in zip(a[r], a[c])]
                inv[r] = [x - fc * y for x, y in zip(inv[r], inv[c])]
    return [x for row in inv for x in row]


def transform(v, m, w=1.0):
    x, y, z = v[0], v[1], v[2]
    return [x * m[0] + y * m[4] + z * m[8] + w * m[12], x * m[1] + y * m[5] + z * m[9] + w * m[13],
            x * m[2] + y * m[6] + z * m[10] + w * m[14], v[3] if len(v) > 3 else 0.0]


class Skeleton:
    """Root data: +0x10 -> local bind matrices M0[n], +0x14 -> world bind matrices M1[n] (row vectors, 4x4 floats),
    +0x1c -> u16 node index per sorted name, +0x20 -> sorted node-name offsets, +0x28 u16 node count, +0x2e u16 name
    count. Verified: M1[b] == M0[b] * M1[parent(b)] exactly, which also yields the parents."""

    def __init__(self, d):
        rd = Tag(d, 0x10).data
        p_m0, p_m1, _p_tex, p_map, p_names = struct.unpack_from('<5I', d, rd + 0x10)
        self.count = struct.unpack_from('<H', d, rd + 0x28)[0]
        n_names = struct.unpack_from('<H', d, rd + 0x2e)[0]
        self.m0 = [list(struct.unpack_from('<16f', d, rd + p_m0 + 64 * i)) for i in range(self.count)]
        self.m1 = [list(struct.unpack_from('<16f', d, rd + p_m1 + 64 * i)) for i in range(self.count)]
        self.names = ['node%d' % i for i in range(self.count)]
        idx = struct.unpack_from('<%dH' % n_names, d, rd + p_map) if n_names else ()
        for i in range(n_names):
            o = rd + struct.unpack_from('<I', d, rd + p_names + 4 * i)[0]
            if idx[i] < self.count:
                self.names[idx[i]] = d[o:d.index(b'\0', o)].decode('ascii', 'replace')
        self.parents = []
        for b in range(self.count):
            best = (max(abs(x - y) for x, y in zip(self.m0[b], self.m1[b])), -1)
            for p in range(self.count):
                if p != b:
                    e = max(abs(x - y) for x, y in zip(mat_mul(self.m0[b], self.m1[p]), self.m1[b]))
                    if e < best[0]:
                        best = (e, p)
            self.parents.append(best[1])
        # skinned parts are stored before ROOTNODE's matrix (an axis permutation); bone-local parts are brought
        # into that same frame with M1[node] * inverse(M1[0])
        self.to_model = [mat_mul(m, mat_inv(self.m1[0])) for m in self.m1] if self.count else []


def node_tags(d):
    """The root's node group: one type-0x2a tag per skeleton node, in matrix order; data = u16 parent (0xffff =
    none), children [0x2b, 0x03 > 0x59 mesh instances owned by the node]."""
    for grp in kids(d, Tag(d, 0x10)):
        ks = kids(d, grp)
        if ks and all(k.type == 0x2a for k in ks):
            return ks
    return []


def mesh_instances(d):
    """Type 0x59 'CombinedMeshInstanceNNNN': {u32 self-rel offset of a u16 list, u16 mesh index, u16 count, ...}.
    Returns {mesh: (owner node, list)}: palette slot i of the mesh maps to node list[i] (an empty list means the
    palette holds node indices directly); parts without a palette are rigid on the owner node."""
    out = {}
    for n, node in enumerate(node_tags(d)):
        for t, _ in walk(d, node):
            if t.type == 0x59 and t.data is not None:
                rel = struct.unpack_from('<I', d, t.data)[0]
                mesh, cnt = struct.unpack_from('<HH', d, t.data + 4)
                out[mesh] = (n, list(struct.unpack_from('<%dH' % cnt, d, t.data + rel)) if cnt else [])
    return out


def inverse_binds(d):
    """{mesh: [matrix per palette slot]} from the type-0x46 child of each 0x2d 'CombinedMeshNNNN' (skinned meshes
    only). 0x46 data: u32 offsets (relative to the data) of [inverse bind matrices, per-bone OBBs (64 bytes each),
    u32 bone-name offsets (relative to the data)], then u16 counts of each. inverse(A[slot]) equals the bone's
    model matrix plus a constant per-mesh translation, so the mesh's vertices can sit in a shifted frame."""
    out = {}
    for t, _ in walk(d, Tag(d, 0x10)):
        if t.type != 0x2d or not t.name[-4:].isdigit():
            continue
        for k in kids(d, t):
            if k.type == 0x46 and k.data is not None:
                rel = struct.unpack_from('<I', d, k.data)[0]
                cnt = struct.unpack_from('<H', d, k.data + 12)[0]
                out[int(t.name[-4:])] = [list(struct.unpack_from('<16f', d, k.data + rel + 64 * i)) for i in range(cnt)]
    return out


def skin_weights(d, part_children):
    """Per-vertex [(slot, weight)] of a skinned part, from its 0x02 > 0x31 records: {u32 8, u16 slot, u16 count}
    + count * {u32 vertex address in bytes (16 per vertex), f32 weight}."""
    out = collections.defaultdict(list)
    for grp in part_children:
        if grp.type != 0x02:
            continue
        for r in kids(d, grp):
            if r.type != 0x31 or r.data is None:
                continue
            hdr, slot, cnt = struct.unpack_from('<IHH', d, r.data)
            for i in range(cnt):
                addr, w = struct.unpack_from('<If', d, r.data + hdr + 8 * i)
                out[addr // 16].append((slot, w))
    return out


def part_contexts(d):
    """Yield (0x56 tag, material index, mesh index, palette or None) for every geometry part."""
    def visit(t, mat, mesh, pal):
        if t.type == 0x4d and t.data is not None:
            mat = struct.unpack_from('<H', d, t.data + 0xc)[0]
        if t.type == 0x2d and t.name[-4:].isdigit():
            mesh = int(t.name[-4:])
        if t.type == 0x4c and t.data is not None:
            pal = list(struct.unpack_from('<%dH' % d[t.data + 4], d, t.data + 8))
        if t.type == 0x56:
            yield t, mat, mesh, pal
            return
        for k in kids(d, t):
            yield from visit(k, mat, mesh, pal)
    yield from visit(Tag(d, 0x10), None, None, None)


def parse_parts(d):
    skel = Skeleton(d)
    inst = mesh_instances(d)
    binds = inverse_binds(d)
    parts = []
    for t, mat_index, mesh, pal in part_contexts(d):
        ch = kids(d, t)
        part = Part()
        part.material = mat_index
        part.node = None
        owner, slots = inst.get(mesh, (None, []))
        skin = None
        if pal is None:
            # rigid mesh: stored in the space of the node that owns the mesh instance
            part.node = owner
        elif len(pal) == 1:
            # single-bone part of a skinned mesh: stored in that bone's space
            part.node = slots[pal[0]] if slots else pal[0]
        elif mesh in binds:
            # skinned part: v * A[slot] * bone, blended by the 0x31 weights. A is the skin's own bind pose, which can
            # differ from the skeleton's rest pose (ACTION/amd), so this poses the skin to match the rigid parts.
            skin = {}
            for s, a in enumerate(binds[mesh]):
                node = slots[s] if slots else s
                if node < skel.count:
                    skin[s] = mat_mul(a, skel.to_model[node])
        if len(ch) >= 5:
            pos33 = first33(d, ch[4]) if ch[4].type == 0x03 else None
            nrm33 = first33(d, ch[2]) if ch[2].type == 0x03 else None
            if pos33 is not None:
                part.positions = block_vectors(d, pos33)
            if nrm33 is not None and part.positions:
                part.normals = block_vectors(d, nrm33, len(part.positions))
            if part.node is not None and part.node < skel.count:
                xf = skel.to_model[part.node]
                part.positions = [transform(v, xf, 1.0) for v in part.positions]
                part.normals = [transform(n, xf, 0.0) for n in part.normals]
            elif skin:
                weights = skin_weights(d, ch)
                fallback = [(pal[0], 1.0)]
                for i, v in enumerate(part.positions):
                    ws = [(s, w) for s, w in weights.get(i, fallback) if s in skin] or fallback
                    total = sum(w for _, w in ws) or 1.0
                    p = [0.0, 0.0, 0.0]
                    n = [0.0, 0.0, 0.0]
                    for s, w in ws:
                        tp = transform(v, skin[s], 1.0)
                        tn = transform(part.normals[i], skin[s], 0.0) if i < len(part.normals) else None
                        for k in range(3):
                            p[k] += tp[k] * w / total
                            if tn:
                                n[k] += tn[k] * w / total
                    part.positions[i] = p + [1.0]
                    if i < len(part.normals):
                        part.normals[i] = n + [0.0]
        for pi, grp in enumerate(g for g in kids(d, ch[0]) if g.type == 0x30) if ch and ch[0].type == 0x02 else []:
            mat = struct.unpack_from('<I', d, grp.data)[0] if grp.data is not None else None
            for lst in kids(d, grp):
                for s in kids(d, lst):
                    if s.type != 0x47 or s.data is None:
                        continue
                    ents = strip_entries(d, s.data)
                    uvs = None
                    for att in kids(d, s):
                        b = first33(d, att)
                        if b is None:
                            continue
                        f = unpack_formats(d, b.data)
                        if f and f[0][0] == 2 and f[0][1] == 0 and f[0][2] == len(ents):
                            uvs = block_vectors(d, b, len(ents))
                    part.strips.append((ents, uvs, pi, mat))
        parts.append((t, part))
    return Tag(d, 0x10), parts


def triangles(ents, flip_rule):
    """Strip -> triangles (local strip vertex indices). ADC (bit 15 of the index) suppresses the triangle ending at
    that vertex. flip_rule picks the winding from the per-vertex bit (bit 15 of the second half-word)."""
    out = []
    for i in range(2, len(ents)):
        a, b = ents[i]
        if a & 0x8000:
            continue
        w = (b >> 15) & 1
        if w ^ flip_rule:
            out.append((i - 2, i - 1, i))
        else:
            out.append((i - 1, i - 2, i))
    return out


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def choose_winding(parts):
    """Return (rule, agree, total): the winding rule whose face normals agree best with the vertex normals."""
    score = [0, 0]
    total = 0
    for _, part in parts:
        if not part.normals:
            continue
        for ents, _uvs, pi, _m in part.strips:
            if pi != 0:
                continue
            for rule in (0, 1):
                for tri in triangles(ents, rule):
                    v = [part.positions[ents[k][0] & 0x7fff] for k in tri]
                    n = cross(sub(v[1], v[0]), sub(v[2], v[0]))
                    vn = [part.normals[ents[k][0] & 0x7fff] for k in tri]
                    avg = (sum(x[0] for x in vn), sum(x[1] for x in vn), sum(x[2] for x in vn))
                    if dot(n, avg) > 0:
                        score[rule] += 1
                    if rule == 0:
                        total += 1
    rule = 0 if score[0] >= score[1] else 1
    return rule, score[rule], total


# ---------------------------------------------------------------- textures
def load_model(path, tex_path=None, model_index=0):
    """A .mdl RTPK holds a texture RTPK and one or more I3D_BIN entries (characters add an untextured low-poly
    companion model with the same skeleton, probably for shadows); model_index picks among the I3D_BIN entries."""
    buf = open(path, 'rb').read()
    textures = {}
    if buf[:4] == rtpk.MAGIC:
        arc = rtpk.Archive(buf)
        models = []
        for e in arc.entries:
            data = bytes(arc.data(e))
            if data[:8] == b'I3D_BIN\0':
                models.append(data)
            elif data[:4] == rtpk.MAGIC:
                textures.update(texture_set(data))
        if not models:
            sys.exit('no I3D_BIN entry in %s' % path)
        if model_index >= len(models):
            sys.exit('%s has %d I3D_BIN models' % (path, len(models)))
        i3d = models[model_index]
    else:
        i3d = buf
    if tex_path:
        textures.update(texture_set(open(tex_path, 'rb').read()))
    return i3d, textures


def texture_set(buf):
    arc = rtpk.Archive(buf)
    out = {}
    for e in arc.entries:
        data = bytes(arc.data(e))
        if data[:4] == b'TIM2':
            out[e.name or '%04d' % e.index] = data
    return out


def texture_names(d):
    """Texture names the model registers (root data: +0x18 -> u32[] of name offsets relative to the root data,
    count u16 at +0x2c; read by 0x22d360 / 0x22d350 once unpacked)."""
    rd = Tag(d, 0x10).data
    arr = rd + struct.unpack_from('<I', d, rd + 0x18)[0]
    n = struct.unpack_from('<H', d, rd + 0x2c)[0]
    out = []
    for i in range(n):
        o = rd + struct.unpack_from('<I', d, arr + 4 * i)[0]
        out.append(d[o:d.index(b'\0', o)].decode('ascii', 'replace'))
    return out


def material_textures(d):
    """Texture name per material index. Materials are the type-0x25 children of the root's material group (in
    order; the u16 at +0xc of a type-0x4d record indexes them). Each material has 0x02 > 0x26 layers whose data
    holds a self-relative pointer at +0x18 to the texture name; 0x21c4d8 (called from DogModel::vfunc_15) looks
    that name up in the registered texture table and stores the index at +0x1c. First named layer wins here."""
    out = []
    for grp in kids(d, Tag(d, 0x10)):
        ks = kids(d, grp)
        if not ks or any(k.type != 0x25 for k in ks):
            continue
        for mat in ks:
            name = None
            for lst in kids(d, mat):
                for layer in kids(d, lst):
                    if layer.type != 0x26 or layer.data is None or name:
                        continue
                    rel = struct.unpack_from('<I', d, layer.data + 0x18)[0]
                    if rel:
                        o = layer.data + rel
                        name = d[o:d.index(b'\0', o)].decode('ascii', 'replace') or None
            out.append(name)
        break
    return out


# ---------------------------------------------------------------- export
def export_obj(d, parts, textures, out_obj, rule):
    base = os.path.splitext(out_obj)[0]
    mtl = base + '.mtl'
    tex_files = {}
    mat_tex = material_textures(d)
    for name in texture_names(d):
        data = textures.get(name)
        if data is None:
            continue
        _, _, pics = tim2png.parse(data)
        png = '%s_%s.png' % (base, name.replace('/', '_'))
        tim2png.write_png(png, pics[0].width, pics[0].height, pics[0].rgba(0))
        tex_files[name] = os.path.basename(png)
    with open(mtl, 'w') as m:
        for i, name in enumerate(mat_tex):
            if name in tex_files:
                m.write('newmtl mat%d_%s\nKd 1 1 1\nmap_Kd %s\n\n' % (i, name, tex_files[name]))
            else:
                m.write('newmtl mat%d_%s\nKd 0.8 0.8 0.8\n\n' % (i, name or 'none'))
        m.write('newmtl untextured\nKd 0.8 0.8 0.8\n')
    vbase = nbase = tbase = 1
    tris_out = 0
    with open(out_obj, 'w') as o:
        o.write('# exported by tools/i3d2obj.py (winding rule %d)\nmtllib %s\n' % (rule, os.path.basename(mtl)))
        for pi_, (tag, part) in enumerate(parts):
            if not part.positions:
                continue
            o.write('o part%d_%x\n' % (pi_, tag.at))
            for v in part.positions:  # Z-up -> Y-up
                o.write('v %.6f %.6f %.6f\n' % (v[0], v[2], -v[1]))
            for n in part.normals:
                o.write('vn %.6f %.6f %.6f\n' % (n[0], n[2], -n[1]))
            for ents, uvs, pass_i, mat in part.strips:
                if pass_i != 0:
                    continue
                mi = part.material if part.material is not None and part.material < len(mat_tex) else None
                o.write('usemtl %s\n' % ('mat%d_%s' % (mi, mat_tex[mi] or 'none') if mi is not None else 'untextured'))
                if uvs:
                    for uv in uvs:
                        o.write('vt %.6f %.6f\n' % (uv[0], 1.0 - uv[1]))
                for tri in triangles(ents, rule):
                    corners = []
                    for k in tri:
                        vi = (ents[k][0] & 0x7fff) + vbase
                        ti = (k + tbase) if uvs else None
                        ni = (ents[k][0] & 0x7fff) + nbase if part.normals else None
                        corners.append('%d/%s/%s' % (vi, ti or '', ni or ''))
                    o.write('f %s\n' % ' '.join(corners))
                    tris_out += 1
                if uvs:
                    tbase += len(uvs)
            vbase += len(part.positions)
            nbase += len(part.normals)
    return tris_out, tex_files


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('model')
    ap.add_argument('--textures')
    ap.add_argument('--dump', action='store_true')
    ap.add_argument('--obj')
    ap.add_argument('--model-index', type=int, default=0, help='which I3D_BIN entry of the .mdl (default 0)')
    a = ap.parse_args()
    d, textures = load_model(a.model, a.textures, a.model_index)
    if d[:8] != b'I3D_BIN\0' or struct.unpack_from('<I', d, 8)[0] != 0x00100001 or d[0x17] & 0x7f != 0x52:
        sys.exit('not an unpacked-on-disc I3D_BIN v1.0x10 root')
    if max(struct.unpack_from('<4I', d, 0x10)[i] for i in (0, 2, 3)) + B >= len(d):
        # FIX/fence.mdl: the entry is cut at 0x4000 bytes while its root tag points past 0xb000
        sys.exit('truncated I3D_BIN: root tag points past the end of the %d-byte file' % len(d))
    root, parts = parse_parts(d)
    rule, agree, total = choose_winding(parts)
    if a.dump:
        for t, part in parts:
            print('part %#x: %d vertices, %d normals, %d strips, passes %s, material words %s' % (
                t.at, len(part.positions), len(part.normals), len(part.strips),
                sorted({s[2] for s in part.strips}), sorted({s[3] for s in part.strips if s[3] is not None})))
        print('textures in model archive:', list(textures))
        print('winding rule %d: %d of %d triangles agree with vertex normals' % (rule, agree, total))
    if a.obj:
        n, tex = export_obj(d, parts, textures, a.obj, rule)
        print('wrote %s: %d triangles, textures %s' % (a.obj, n, list(tex.values())))
    if not (a.dump or a.obj):
        ap.print_help()


if __name__ == '__main__':
    main()
