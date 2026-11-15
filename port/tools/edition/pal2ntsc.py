#!/usr/bin/env python3
"""
PAL -> NTSC-US address translation for HP: Philosopher's / Sorcerer's Stone.

The two retail XBEs are separate links of the same source (same title ID 45410037, same signature key):
  * every section after .text holds the same bytes 0x20 higher (.text is 32 bytes longer in NTSC), but the
    uninitialised end of .data (BSS) is laid out differently: runs at +0x24 and +0x28 (map's 'data', from the operands);
  * .text functions are reordered in blocks (shifts -0x5E0 .. +0x8E0), so no single offset works.

`map` matches every PAL .text function to NTSC by its bytes with the address operands masked (rel32 call/jmp/jcc
targets, absolute displacements and immediates inside the image), then fills small functions from their matched
neighbours' shift. An address inside a matched function keeps its offset (masked matching implies the same
instruction layout).

`translate` rewrites sub_/loc_XXXXXXXX names and 0x literals in the given files and prints every change; a literal that
is not an instruction start inside a matched function, or a round number that could be a size, is listed under
REVIEW and left alone unless --force-review.

    python3 port/tools/edition/pal2ntsc.py map PAL.xbe NTSC.xbe pal_functions.json -o map.json
    python3 port/tools/edition/pal2ntsc.py translate map.json FILE... [--dry-run]

A fix written for the PAL port (hpps) is carried over by running `translate` on the changed file.
"""
import argparse, bisect, collections, json, re, struct, sys

TEXT = 0x11000
PAL_TEXT_END = 0x190714          # PAL .text VA end (0x11000 + 1,570,580)
IMAGE_END = 0x37F2A8             # PAL .XTLID end: last section of the image
LATE_SHIFT = 0x20                # every section after .text


def load_xbe(path):
    d = open(path, 'rb').read()
    base = struct.unpack_from('<I', d, 0x104)[0]
    ns = struct.unpack_from('<I', d, 0x11C)[0]
    sh = struct.unpack_from('<I', d, 0x120)[0] - base
    secs = {}
    for i in range(ns):
        _fl, va, vs, ra, rs, na = struct.unpack_from('<IIIIII', d, sh + i * 56)
        name = d[na - base:d.index(b'\0', na - base)].decode()
        secs[name] = (va, vs, d[ra:ra + rs])
    return secs


class Masker:
    def __init__(self, text):
        from capstone import Cs, CS_ARCH_X86, CS_MODE_32
        self.md = Cs(CS_ARCH_X86, CS_MODE_32)
        self.md.detail = True
        self.text = text

    def masked(self, start, size):
        code = self.text[start - TEXT:start - TEXT + size]
        mask = bytearray(b'\x01' * len(code))
        starts = set()
        for ins in self.md.disasm(code, start):
            o = ins.address - start
            starts.add(ins.address)
            b0 = ins.bytes[0]
            if b0 in (0xE8, 0xE9) and ins.size == 5 or b0 == 0x0F and ins.size == 6 and 0x80 <= ins.bytes[1] <= 0x8F:
                mask[o + ins.size - 4:o + ins.size] = b'\0\0\0\0'
                continue
            for off, sz in ((ins.disp_offset, ins.disp_size), (ins.imm_offset, ins.imm_size)):
                if sz == 4:
                    v = struct.unpack_from('<I', code, o + off)[0]
                    if 0x10000 <= v < 0x400000:
                        mask[o + off:o + off + 4] = b'\0\0\0\0'
        return code, mask, starts


def build_map(pal_xbe, ntsc_xbe, funcs_json):
    P = load_xbe(pal_xbe)['.text'][2]
    N = load_xbe(ntsc_xbe)['.text'][2]
    mk = Masker(P)
    funcs = sorted((f for f in json.load(open(funcs_json)) if f['section'] == '.text'), key=lambda f: int(f['start'], 16))
    K = 12
    idx = collections.defaultdict(list)
    for i in range(len(N) - K):
        idx[N[i:i + K]].append(i)

    def fits(code, mask, b):
        o = b - TEXT
        if o < 0 or o + len(code) > len(N):
            return False
        return all(m == 0 or N[o + k] == code[k] for k, m in enumerate(mask))

    m = {}
    for f in funcs:
        a = int(f['start'], 16)
        code, mask, _ = mk.masked(a, f['size'])
        key = next((i for i in range(max(0, len(code) - K)) if all(mask[i:i + K])), None)
        if key is None:
            continue
        cands = [j - key + TEXT for j in idx.get(bytes(code[key:key + K]), []) if fits(code, mask, j - key + TEXT)]
        if cands:
            m[a] = min(cands, key=lambda c: abs(c - a))
    for _ in range(3):  # neighbours' shift for short functions and to settle ambiguous ones
        for i, f in enumerate(funcs):
            a = int(f['start'], 16)
            deltas = []
            for d in (-1, 1, -2, 2):
                j = i + d
                if 0 <= j < len(funcs) and int(funcs[j]['start'], 16) in m:
                    aj = int(funcs[j]['start'], 16)
                    deltas.append(m[aj] - aj)
            if a in m and m[a] - a in deltas[:2]:
                continue
            code, mask, _ = mk.masked(a, f['size'])
            for dl in deltas:
                if fits(code, mask, a + dl):
                    m[a] = a + dl
                    break
    # the operands of every matched pair: call targets vote for the callee's NTSC address (settles look-alike short
    # functions and places some the bytes could not), data addresses give the shift of each data region (BSS is laid
    # out differently: +0x24 / +0x28 runs inside .data's uninitialised part)
    def operands():
        for f in funcs:
            a = int(f['start'], 16)
            n = m.get(a)
            if n is None:
                continue
            code, mask, _ = mk.masked(a, f['size'])
            if not fits(code, mask, n):  # placed by votes alone: its bytes differ, so its operands don't pair up
                continue
            k = 0
            while k < len(code):
                if mask[k] == 0:
                    pv = struct.unpack_from('<I', code, k)[0]
                    nv = struct.unpack_from('<I', N, n - TEXT + k)[0]
                    rel = k >= 1 and (code[k - 1] in (0xE8, 0xE9) or k >= 2 and code[k - 2] == 0x0F and 0x80 <= code[k - 1] <= 0x8F)
                    yield rel, a + k, n + k, pv, nv
                    k += 4
                else:
                    k += 1
    starts = {int(f['start'], 16) for f in funcs}
    for _ in range(2):
        votes = collections.defaultdict(collections.Counter)
        for rel, pa, na, pv, nv in operands():
            if rel:
                pt, nt = (pa + 4 + pv) & 0xFFFFFFFF, (na + 4 + nv) & 0xFFFFFFFF
                if pt in starts:
                    votes[pt][nt] += 1
        fixed = 0
        for pt, c in votes.items():
            (nt, cnt), = c.most_common(1)
            if len(c) == 1 and m.get(pt) != nt and TEXT <= nt < TEXT + len(N):
                m[pt] = nt
                fixed += 1
        print(f'call votes set {fixed} functions', file=sys.stderr)
    # every operand that points into the image, as the NTSC code has it: exact answers for the addresses the code
    # itself uses (function pointers, jump tables, odd data spots), checked before any rule
    seen = collections.defaultdict(collections.Counter)
    for rel, pa, na, pv, nv in operands():
        if rel:
            pv, nv = (pa + 4 + pv) & 0xFFFFFFFF, (na + 4 + nv) & 0xFFFFFFFF
        if TEXT <= pv < IMAGE_END:
            seen[pv][nv] += 1
    exact = {pv: next(iter(c)) for pv, c in seen.items() if len(c) == 1}
    moved = 0
    for a in starts:
        nt = exact.get(a)
        if nt is not None and TEXT <= nt < TEXT + len(N) and m.get(a) != nt:
            m[a] = nt
            moved += 1
    print(f'pointers set {moved} functions, {len(exact)} exact operands', file=sys.stderr)
    obs = collections.defaultdict(collections.Counter)
    for rel, pa, na, pv, nv in operands():
        if not rel and PAL_TEXT_END <= pv < IMAGE_END:
            obs[pv][nv - pv] += 1
    runs = []
    for pv in sorted(obs):
        d, cnt = obs[pv].most_common(1)[0]
        if runs and runs[-1][2] == d:
            runs[-1][1] = pv
            runs[-1][3] += cnt
        else:
            runs.append([pv, pv, d, cnt])
    # a lone address with its own shift is a constant that looks like one (0x269EC3, rand's increment)
    runs = [r for r in runs if r[2] == LATE_SHIFT or r[1] > r[0]]
    merged = []
    for r in runs:
        if merged and merged[-1][2] == r[2]:
            merged[-1][1] = r[1]
        else:
            merged.append(r[:3])
    for r in merged:
        print(f'data {r[0]:#x}-{r[1]:#x} {r[2]:+#x}', file=sys.stderr)
    out = {'functions': [[int(f['start'], 16), int(f['end'], 16), m.get(int(f['start'], 16))] for f in funcs],
           'data': [[r[0], r[2]] for r in merged],
           'exact': [[k, v] for k, v in sorted(exact.items())]}
    print(f'matched {sum(1 for f in out["functions"] if f[2])} of {len(funcs)} .text functions', file=sys.stderr)
    return out


class Translator:
    def __init__(self, mp, pal_xbe=None):
        self.f = mp['functions']
        self.data = mp.get('data') or [[PAL_TEXT_END, LATE_SHIFT]]
        self.dstarts = [d[0] for d in self.data]
        self.exact = dict(map(tuple, mp.get('exact', [])))
        self.starts = [f[0] for f in self.f]
        self.mk = Masker(load_xbe(pal_xbe)['.text'][2]) if pal_xbe else None

    def tr(self, v):
        """-> (new value or None, note)"""
        if v in self.exact:
            return self.exact[v], 'operand'
        if PAL_TEXT_END <= v < IMAGE_END:
            i = bisect.bisect_right(self.dstarts, v) - 1
            d = self.data[i][1] if i >= 0 else LATE_SHIFT
            return v + d, 'late' if d == LATE_SHIFT else f'data {d:+#x}'
        if TEXT <= v < PAL_TEXT_END:
            i = bisect.bisect_right(self.starts, v) - 1
            if i < 0 or not (self.f[i][0] <= v < self.f[i][1]):
                return None, 'outside any function'
            s, e, n = self.f[i]
            if n is None:
                return None, f'in unmatched sub_{s:08X}'
            if v != s and self.mk:
                _, _, starts = self.mk.masked(s, e - s)
                if v not in starts:
                    return v - s + n, f'mid-instruction in sub_{s:08X}'
            return v - s + n, 'start' if v == s else f'sub_{s:08X}+0x{v - s:X}'
        if v == PAL_TEXT_END:
            return PAL_TEXT_END + LATE_SHIFT, '.text end'
        return None, 'not an image address'


SUB = re.compile(r'\b(sub|loc)_([0-9A-Fa-f]{8})(?![0-9A-Fa-f])')
HEX = re.compile(r'\b0x([0-9A-Fa-f]{5,8})(?![0-9A-Fa-f])')
# a function address passed bare to a token-pasting macro: FALLBACK(001A89A2) -> sub_##addr##_gen (hle_dsound.c,
# hle_d3d8.cpp's HLE(addr, nargs))
BARE = re.compile(r'(?<=\()(\s*)(00[0-9A-Fa-f]{6})(?=\s*[,)])')


def fmt(new, old_digits):
    s = f'{new:0{len(old_digits)}X}'
    return s if not old_digits.islower() or old_digits.isdigit() else s.lower()


def load_rules(path):
    """rules.txt: `keep 0xV` leaves a literal as it is, `map 0xPAL 0xNTSC` translates one the map cannot place."""
    keep, fixed = set(), {}
    if path:
        for l in open(path):
            l = l.split('#', 1)[0].split()
            if not l:
                continue
            if l[0] == 'keep':
                keep.add(int(l[1], 16))
            elif l[0] == 'map':
                fixed[int(l[1], 16)] = int(l[2], 16)
            else:
                sys.exit(f'{path}: unknown rule {l[0]!r}')
    return keep, fixed


def translate_text(text, T, keep, fixed, force_review=False, name='?'):
    changes, review = [], []

    def line_of(pos):
        return text.count('\n', 0, pos) + 1

    def rep_sub(mo):
        v = int(mo.group(2), 16)
        if v in keep:
            return mo.group(0)
        n, note = (fixed[v], 'rules.txt') if v in fixed else T.tr(v)
        if n is None:
            if TEXT <= v < IMAGE_END:
                review.append((line_of(mo.start()), mo.group(0), note))
            return mo.group(0)
        new = f'{mo.group(1)}_{fmt(n, mo.group(2))}'
        changes.append((line_of(mo.start()), mo.group(0), new, note))
        return new

    def rep_hex(mo):
        v = int(mo.group(1), 16)
        if not (TEXT <= v < IMAGE_END + 1) or v in keep:
            return mo.group(0)
        if v in fixed:
            n, note, doubt = fixed[v], 'rules.txt', False
        else:
            n, note = T.tr(v)
            round_ = v & 0xFFF == 0
            doubt = n is None or note.startswith('mid-instruction') or round_
            if round_:
                note += ' (round number: size?)'
        if doubt and not force_review:
            review.append((line_of(mo.start()), mo.group(0), note))
            return mo.group(0)
        new = '0x' + fmt(n, mo.group(1))
        changes.append((line_of(mo.start()), mo.group(0), new, note))
        return new

    def rep_bare(mo):
        v = int(mo.group(2), 16)
        if not (TEXT <= v < IMAGE_END) or v in keep:
            return mo.group(0)
        n, note = (fixed[v], 'rules.txt') if v in fixed else T.tr(v)
        if n is None:
            review.append((line_of(mo.start()), mo.group(2), note))
            return mo.group(0)
        changes.append((line_of(mo.start()), mo.group(2), fmt(n, mo.group(2)), note))
        return mo.group(1) + fmt(n, mo.group(2))

    out = SUB.sub(rep_sub, text)
    out = HEX.sub(rep_hex, out)
    out = BARE.sub(rep_bare, out)
    return out, changes, review


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest='cmd', required=True)
    a = sp.add_parser('map')
    a.add_argument('pal'); a.add_argument('ntsc'); a.add_argument('functions'); a.add_argument('-o', required=True)
    t = sp.add_parser('translate')
    t.add_argument('map'); t.add_argument('files', nargs='+')
    t.add_argument('--pal', help='PAL XBE: flags literals that are not instruction starts')
    t.add_argument('--rules', help='rules.txt: literals to keep, and PAL -> NTSC pairs the map cannot place')
    t.add_argument('--root', help='with --out-dir: the files are written at their path relative to this')
    t.add_argument('--out-dir', help='write the NTSC copies here instead of over the files')
    t.add_argument('--strict', action='store_true', help='fail on a literal neither translated nor in rules.txt')
    t.add_argument('--quiet', action='store_true', help='print only what needs review')
    t.add_argument('--dry-run', action='store_true')
    t.add_argument('--force-review', action='store_true', help='translate the REVIEW literals too')
    args = ap.parse_args()
    if args.cmd == 'map':
        json.dump(build_map(args.pal, args.ntsc, args.functions), open(args.o, 'w'))
        return
    import os
    T = Translator(json.load(open(args.map)), args.pal)
    keep, fixed = load_rules(args.rules)
    bad = 0
    for p in args.files:
        text = open(p, encoding='utf-8', errors='surrogateescape').read()
        out, ch, rv = translate_text(text, T, keep, fixed, args.force_review, p)
        if not args.quiet:
            for ln, old, new, note in ch:
                print(f'{p}:{ln}: {old} -> {new}  [{note}]')
        for ln, old, note in rv:
            print(f'REVIEW {p}:{ln}: {old}  [{note}]', file=sys.stderr if args.strict else sys.stdout)
        bad += len(rv)
        if args.dry_run:
            continue
        dst = os.path.join(args.out_dir, os.path.relpath(p, args.root or '.')) if args.out_dir else p
        os.makedirs(os.path.dirname(dst) or '.', exist_ok=True)
        if args.out_dir and dst.endswith(('.c', '.cpp', '.h')):
            out = f'/* NTSC-US copy of {os.path.relpath(p, args.root or ".")}, made by port/tools/edition/pal2ntsc.py: edit the PAL source */\n' + out
        if not (os.path.exists(dst) and open(dst, encoding='utf-8', errors='surrogateescape').read() == out):
            open(dst, 'w', encoding='utf-8', errors='surrogateescape', newline='').write(out)
    if args.strict and bad:
        sys.exit(f'pal2ntsc: {bad} literal(s) need a rule in port/tools/edition/rules.txt (keep or map)')


if __name__ == '__main__':
    main()
