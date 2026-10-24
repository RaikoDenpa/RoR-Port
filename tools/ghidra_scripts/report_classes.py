"""Format RoRClasses.java reports (classes.tsv, vtables.tsv) into the tables of notes/classes.md.

Usage: python report_classes.py <report_dir> [--dump]
  Writes the generated markdown to stdout. --dump prints raw parsed rows instead.
Only slots a class introduces or overrides are listed; inherited slots are summarised.
"""
import csv
import sys
from collections import defaultdict


def load(path):
    with open(path, encoding='utf-8', newline='') as f:
        return list(csv.DictReader(f, delimiter='\t'))


def main():
    rep = sys.argv[1]
    classes = load(rep + '/classes.tsv')
    slots = load(rep + '/vtables.tsv')
    if '--dump' in sys.argv:
        for r in classes:
            print(r)
        return

    by_name = {c['class']: c for c in classes}
    base = {c['class']: (c['bases'].split('@')[0] if c['bases'] else None) for c in classes}
    children = defaultdict(list)
    for c in classes:
        children[base[c['class']]].append(c['class'])

    vt = defaultdict(list)  # vtable addr -> slots
    for s in slots:
        vt[s['vtable']].append(s)

    def primary_slots(cls):
        v = by_name[cls]['vtables'].split(';')[0] if by_name[cls]['vtables'] else None
        return {int(s['slot']): s for s in vt.get(v, [])} if v else {}

    def depth(c):
        d = 0
        while base.get(c):
            c = base[c]
            d += 1
        return d

    out = []
    out.append('## Hierarchy\n')
    out.append('```')

    def tree(c, ind):
        info = by_name[c]
        vts = info['vtables'] or 'no vtable'
        out.append('%s%s  [vt %s]' % ('  ' * ind, c, vts))
        for k in sorted(children.get(c, [])):
            tree(k, ind + 1)
    for r in sorted(children[None]):
        tree(r, 0)
    out.append('```\n')

    out.append('## Classes\n')
    out.append('| Class | Base chain | vtable | __tf | __ti | slots |')
    out.append('|---|---|---|---|---|---|')
    for c in sorted(by_name, key=lambda x: x.lower()):
        chain, b = [], base[c]
        while b:
            chain.append(b)
            b = base.get(b)
        n = len(primary_slots(c))
        info = by_name[c]
        out.append('| %s | %s | %s | %s | %s | %s |' % (
            c, ' → '.join(chain) or '—', info['vtables'] or '—', info['tf'], info['ti'], max(n - 1, 0) if n else '—'))
    out.append('')

    abs_path = rep + '/abstract_slots.tsv'
    try:
        abs_rows = load(abs_path)
    except FileNotFoundError:
        abs_rows = []
    if abs_rows:
        out.append('## Slots of bases without a vtable, and pattern-named slots\n')
        out.append('From `abstract_slots.tsv`. `abstract`: the slot provably belongs to the base and all descendants share '
                   'its name. `pattern`: named from the Init/Exec/Term order of the adjacent slots. `pattern-agrees`: '
                   'the pattern confirms a direct name.\n')
        out.append('| Rule | Class | Slot | Name | Evidence |')
        out.append('|---|---|---|---|---|')
        for r in abs_rows:
            out.append('| %s | %s | %s | %s | %s |' % (r['rule'], r['class'], r['slot'], r['name'], r['evidence']))
        out.append('')

    out.append('## Virtual methods by slot\n')
    out.append('Slot 0 is always the class\'s `__tf`. Only slots the class introduces or overrides are listed; '
               '`vfunc_N` = unnamed function first used by that class in slot N.\n')
    for c in sorted(by_name, key=lambda x: (depth(x), x.lower())):
        mine = primary_slots(c)
        if not mine:
            continue
        b = base[c]
        while b and not primary_slots(b):
            b = base.get(b)
        theirs = primary_slots(b) if b else {}
        rows = [s for i, s in sorted(mine.items()) if i > 0 and (i not in theirs or theirs[i]['pfn'] != s['pfn'])]
        inherited = sum(1 for i, s in mine.items() if i > 0 and i in theirs and theirs[i]['pfn'] == s['pfn'])
        out.append('### %s  (vt %s, %d slots; %d inherited from %s)\n' % (
            c, by_name[c]['vtables'].split(';')[0], len(mine) - 1, inherited, b or '—'))
        if rows:
            out.append('| Slot | Function | Address | Overrides |')
            out.append('|---|---|---|---|')
            for s in rows:
                i = int(s['slot'])
                ov = theirs[i]['function'] if i in theirs else 'new'
                out.append('| %d | `%s` | %s | %s |' % (i, s['function'], s['pfn'], ov if ov == 'new' else '`%s`' % ov))
        out.append('')
    sys.stdout.reconfigure(encoding='utf-8')
    print('\n'.join(out))


if __name__ == '__main__':
    main()
