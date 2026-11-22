"""Test of tools/lifter/jumptables.py: the case list of a lifted function's `br` dispatch (RECOMP_JT).

    python tests/jumptables_test.py                 synthetic functions in a fake image (no game binary needed)
    RECOMP_ELF=<main.elf> python tests/jumptables_test.py --real [--all]
                                                    also real functions of the game: a sample of the functions holding a
                                                    br (every 8th and the 40 longest; --all: every one), lifted both ways

Synthetic: the compiler's jump-table shape is proven and its cases are exactly the table's in-function targets (+ the
entry); each thing that breaks the proof leaves the full switch: a 32-bit bound on a register whose upper half is
unknown, a flag-setting instruction between the cmp and the b.hi, the base clobbered by a call (a caller-saved register;
a callee-saved one holds), a branch into the window, a table in writable data, under a relocation or written by a site
(MUTABLE), FORCE_FULL; a target outside the function is no case; level 2 takes a br of a pointer loaded from memory as a
tail call, level 1 does not; RECOMP_JT=0 prints the old switch byte for byte, the reduced switch traps on an in-function
target it does not list.
Real (--real): for every lifted copy of the sample, (1) RECOMP_JT=0 gives the old text: a case per instruction and the
old default line; (2) with RECOMP_JT=1 and 2 the instruction lines are the same as with 0, only the dispatch block
differs; (3) every target that an INDEPENDENT pattern matcher (below: the clang table shape, no dataflow) reads for a br
is a case of the reduced switch, or the function kept the full switch; where both resolve a br the analysis reads at
least the matcher's targets (more where another path reaches the table code with a looser bound); (4) every case is the entry or a target the analysis proved; (5) the default line traps on [entry, end).
"""
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'tools', 'lifter'))
REAL = '--real' in sys.argv
if not REAL:
    os.environ['RECOMP_NO_IMAGE'] = '1'
os.environ.pop('RECOMP_PATCHES', None)
import recomp          # noqa: E402
import jumptables as J  # noqa: E402

FAILS = []


def check(cond, what):
    print('%s  %s' % ('ok  ' if cond else 'FAIL', what))
    if not cond:
        FAILS.append(what)


# ------------------------------------------------------------------------------------------------ encoders
def adrp(rd, page): return 0x90000000 | ((page & 3) << 29) | (((page >> 2) & 0x7ffff) << 5) | rd
def addi(rd, rn, imm, sf=1): return (0x91000000 if sf else 0x11000000) | (imm << 10) | (rn << 5) | rd
def subi_w(rd, rn, imm): return 0x51000000 | (imm << 10) | (rn << 5) | rd
def cmpi(rn, imm, sf=0): return (0xf1000000 if sf else 0x71000000) | (imm << 10) | (rn << 5) | 31
def bcond(here, to, cond): return 0x54000000 | ((((to - here) // 4) & 0x7ffff) << 5) | cond
def b(here, to): return 0x14000000 | (((to - here) // 4) & 0x3ffffff)
def bl(here, to): return 0x94000000 | (((to - here) // 4) & 0x3ffffff)
def ldrsw_reg(rt, rn, rm): return 0xb8a07800 | (rm << 16) | (rn << 5) | rt
def addr(rd, rn, rm): return 0x8b000000 | (rm << 16) | (rn << 5) | rd
def movw(rd, rm): return 0x2a0003e0 | (rm << 16) | rd
def ldrx(rt, rn, off): return 0xf9400000 | ((off // 8) << 10) | (rn << 5) | rt
def br(rn): return 0xd61f0000 | (rn << 5)
RET, NOP, ADDS = 0xd65f03c0, 0xd503201f, 0xb1000421            # adds x1, x1, #1


# ------------------------------------------------------------------------------------------------ fake image
BASE, FO = recomp.BASE, 0x1000
TEXT, RO, DATA = 0x0, 0x10000, 0x20000              # segment offsets from BASE (file offset = FO + offset)


def image(code, tables, relocs=()):
    """code: {offset: word}; tables: {offset: [int32 ...]} anywhere; relocs: offsets a RELA entry writes"""
    img = bytearray(FO + 0x30000)
    struct.pack_into('<Q', img, 0x20, 0x40)
    struct.pack_into('<H', img, 0x38, 3)
    for i, (flags, off, size) in enumerate(((5, TEXT, 0x10000), (4, RO, 0x10000), (6, DATA, 0x10000))):
        struct.pack_into('<IIQQQQQQ', img, 0x40 + i * 56, 1, flags, FO + off, off, off, size, size, 0x1000)
    for o, w in code.items():
        struct.pack_into('<I', img, FO + o, w)
    for o, vals in tables.items():
        for k, v in enumerate(vals):
            struct.pack_into('<i', img, FO + o + 4 * k, v)
    rela = DATA + 0x8000
    for k, o in enumerate(relocs):
        struct.pack_into('<QQq', img, FO + rela + 24 * k, o, 0x403, 0)
    recomp.IMG = bytes(img)
    recomp.RELA, recomp.RELASZ, recomp.JMPREL, recomp.PLTRELSZ = rela, 24 * len(relocs), 0, 0
    recomp.TEXT_SIZE = 0x10000
    J._IMAGES.clear()


def lift(entry, end):
    env = recomp.Env()
    env.full = True
    env.imports = set()
    fn = recomp.Fn(entry, end, env)
    return [(pc, fn.lift(recomp.word(pc), pc)) for pc in range(entry, end, 4)]


def switch_fn(pre=(), bound=None, base_reg=9, between_base=(), table_at=RO + 0x100, idx=8, mov=True, after_cmp=(), n=3):
    """f at BASE+0x1000: [pre] mov w8,w0; cmp w8,#n-1; b.hi def; adrp/add base; [between_base]; ldrsw; add; br;
    cases: n blocks of `ret` after it. Returns (code, tables, entry, end, case addresses)."""
    e = 0x1000
    ins = list(pre)
    if mov:
        ins.append(movw(idx, 0))
    ins.append(('cmp',))
    ins += list(after_cmp)
    ins.append(('bhi',))
    ins += [('adrp',), ('add',)] + list(between_base) + [ldrsw_reg(10, base_reg, idx), addr(10, 10, base_reg), br(10)]
    k_def = len(ins)
    ins.append(RET)                                                  # default
    cases = []
    for _ in range(n):
        cases.append(e + 4 * len(ins))
        ins += [NOP, RET]
    code = {}
    for k, w in enumerate(ins):
        pc = e + 4 * k
        if w == ('cmp',):
            w = cmpi(idx if bound is None else bound, n - 1)
        elif w == ('bhi',):
            w = bcond(pc, e + 4 * k_def, 8)
        elif w == ('adrp',):
            w = adrp(base_reg, ((BASE + table_at) >> 12) - ((BASE + pc) >> 12))
        elif w == ('add',):
            w = addi(base_reg, base_reg, table_at & 0xfff)
        code[e + 4 * k] = w
    tables = {table_at: [(BASE + c) - (BASE + table_at) for c in cases]}
    return code, tables, BASE + e, BASE + e + 4 * len(ins), [BASE + c for c in cases]


def run(code, tables, entry, end, relocs=(), level=1, pads=()):
    image(code, tables, relocs)
    lines = lift(entry, end)
    return J.analyze(recomp, entry, end, lines, pads, level), lines


def synthetic():
    code, tables, e, end, cases = switch_fn()
    (got, sites), lines = run(code, tables, e, end)
    check(got == sorted(cases + [e]), 'clang shape (mov w8, w0; cmp; b.hi; adrp/add; ldrsw; add; br): cases = table targets + entry')
    check(list(sites.values())[0][0] == 'table', '  the br is a proven table')

    code, tables, e, end, _ = switch_fn(mov=False, idx=0)
    check(run(code, tables, e, end)[0][0] is None, 'cmp w0 with x0 an argument (upper half unknown), lsl index: full switch')

    code, tables, e, end, cases = switch_fn(pre=[subi_w(8, 0, 1)], mov=False)
    check(run(code, tables, e, end)[0][0] == sorted(cases + [e]), 'sub w8, w0, #1 (a 32-bit write) then cmp w8: proven')

    code, tables, e, end, _ = switch_fn(after_cmp=[ADDS])
    check(run(code, tables, e, end)[0][0] is None, 'a flag-setting adds between cmp and b.hi: full switch')
    code, tables, e, end, _ = switch_fn(after_cmp=[movw(3, 4)])
    check(run(code, tables, e, end)[0][0] is not None, 'a mov between cmp and b.hi (no flags, index not written): proven')
    code, tables, e, end, _ = switch_fn(after_cmp=[movw(8, 4)])
    check(run(code, tables, e, end)[0][0] is None, 'the index written between cmp and b.hi: full switch')

    code, tables, e, end, _ = switch_fn(between_base=[('bl',)])
    for o, w in list(code.items()):
        if w == ('bl',):
            code[o] = bl(o, 0x8000)
    check(run(code, tables, e, end)[0][0] is None, 'base in x9 (caller-saved) across a bl: full switch')
    code, tables, e, end, cases = switch_fn(between_base=[('bl',)], base_reg=19, idx=20)
    for o, w in list(code.items()):
        if w == ('bl',):
            code[o] = bl(o, 0x8000)
    check(run(code, tables, e, end)[0][0] == sorted(cases + [e]), 'base x19 and index x20 (callee-saved) across a bl: proven')

    code, tables, e, end, cases = switch_fn(pre=[NOP, NOP])
    ld = next(o for o in sorted(code) if (code[o] & 0xffe0fc00) == 0xb8a07800)
    code[0x1000] = b(0x1000, ld)                                     # entry: b to the ldrsw (index unbounded on that path)
    code[0x1004] = RET
    check(run(code, tables, e, end)[0][0] is None, 'a branch into the window from a path without the bound: full switch')

    t = RO + 0x200                                                   # the bound of a b.hs holds on its fall-through only
    code = {0x1000: movw(20, 0), 0x1004: cmpi(20, 3), 0x1008: bcond(0x1008, 0x1010, 2), 0x100c: NOP,
            0x1010: adrp(9, ((BASE + t) >> 12) - ((BASE + 0x1010) >> 12)), 0x1014: addi(9, 9, t & 0xfff),
            0x1018: ldrsw_reg(10, 9, 20), 0x101c: addr(10, 10, 9), 0x1020: br(10), 0x1024: RET, 0x1028: RET, 0x102c: RET}
    tabs = {t: [(BASE + 0x1024 + 4 * k) - (BASE + t) for k in range(3)]}
    check(run(code, tabs, BASE + 0x1000, BASE + 0x1030)[0][0] is None,
          'b.hs taken to the table code (index >= 3 there): no bound at the join, full switch')
    code[0x1008] = bcond(0x1008, 0x1030, 2)                          # taken path leaves: now the fall-through alone reaches it
    code[0x100c] = b(0x100c, 0x1010)
    check(run(code, tabs, BASE + 0x1000, BASE + 0x1030)[0][0] == [BASE + 0x1000, BASE + 0x1024, BASE + 0x1028, BASE + 0x102c],
          '  b.hs out of the function, b to the table code: the bound holds across the join, proven')

    code, tables, e, end, _ = switch_fn(table_at=DATA + 0x100)
    check(run(code, tables, e, end)[0][0] is None, 'table in writable data: full switch')
    code, tables, e, end, _ = switch_fn()
    check(run(code, tables, e, end, relocs=[RO + 0x104])[0][0] is None, 'a relocation inside the table: full switch')
    J.MUTABLE[:] = [(BASE + RO + 0x108, BASE + RO + 0x10c)]
    check(run(code, tables, e, end)[0][0] is None, 'a site writes into the table (MUTABLE): full switch')
    J.MUTABLE[:] = []
    J.FORCE_FULL.add(e + 8)
    check(run(code, tables, e, end)[0][0] is None, 'FORCE_FULL address inside the function: full switch')
    J.FORCE_FULL.clear()

    code, tables, e, end, cases = switch_fn()
    tables[RO + 0x100][1] = (BASE + 0x9000) - (BASE + RO + 0x100)    # one entry outside the function
    (got, sites), _ = run(code, tables, e, end)
    check(got == sorted([cases[0], cases[2], e]), 'a table target outside the function is no case (call_indirect, as before)')

    code = {0x1000: ldrx(8, 0, 8), 0x1004: br(8)}
    e, end = BASE + 0x1000, BASE + 0x1008
    check(run(code, {}, e, end, level=1)[0][0] is None, 'ldr x8, [x0, #8]; br x8 at level 1: full switch')
    (got, sites), _ = run(code, {}, e, end, level=2)
    check(got == [e] and list(sites.values())[0][0] == 'tail', '  at level 2: a tail call, the entry is the only case')

    # the dispatch text
    code, tables, e, end, cases = switch_fn()
    image(code, tables)
    lines = lift(e, end)
    J.LEVEL = 0
    old = J.dispatch(recomp, e, end, lines)
    want = ['L_dispatch: switch (jt) {'] + ['  case 0x%xULL: goto L_%x;' % (pc, pc) for pc, _ in lines] + \
           ['  default: call_indirect(c, jt); return; }']
    check(old == want, 'RECOMP_JT=0: the old switch byte for byte')
    J.LEVEL = 1
    new = J.dispatch(recomp, e, end, lines)
    check(new[1:-1] == ['  case 0x%xULL: goto L_%x;' % (t, t) for t in sorted(cases + [e])], 'RECOMP_JT=1: a case per target')
    check(new[-1].startswith('  default: if (jt - 0x%xULL < 0x%xULL) guest_trap(' % (e, end - e)) and
          new[-1].endswith('call_indirect(c, jt); return; }'), '  default: trap inside [entry, end), else call_indirect')
    J.LEVEL = 0


# ------------------------------------------------------------------------------------------------ real binary
def independent_targets(word, img_ro, a, entry, end):
    """the clang shape read backwards from the br at a, without jumptables.py: add xT, xL, xB / xB, xL; ldrsw xL,
    [xB, xI, lsl #2]; adrp+add xB (anywhere in the 40 instructions before, the last write of xB); cmp wI|xI, #N (the last one) with a
    b.hi after it. None if any piece is missing. A heuristic, not a proof: what the analysis must cover."""
    w = word(a)
    t = (w >> 5) & 31
    k, base, idx, ld, lo, page, n = a - 4, None, None, None, None, None, None
    stop = max(entry, a - 160)
    while k >= stop and (base is None or idx is None or page is None or n is None):
        v = word(k)
        rd = v & 31
        if ld is None and (v & 0xffe0fc00) == 0x8b000000 and rd == t:
            ra, rb = (v >> 5) & 31, (v >> 16) & 31
            ld = (ra, rb)
        elif ld and base is None and (v & 0xffe0fc00) == 0xb8a07800 and rd in ld:
            base, idx = (v >> 5) & 31, (v >> 16) & 31
            if base not in ld:
                return None
        elif base is not None and lo is None and (v & 0xffc00000) == 0x91000000 and rd == base and (v >> 5) & 31 == base:
            lo = (v >> 10) & 0xfff
        elif lo is not None and page is None and (v & 0x9f000000) == 0x90000000 and rd == base:
            imm = ((((v >> 5) & 0x7ffff) << 2) | ((v >> 29) & 3))
            imm -= (1 << 21) if imm >> 20 else 0
            page = (k & ~0xfff) + (imm << 12) + lo
        elif idx is not None and n is None and (v & 0x7fc0001f) == 0x7100001f and (v >> 5) & 31 == idx:
            if not any((word(j) & 0xff00001f) == 0x54000008 for j in range(k + 4, a, 4)):
                return None                                          # the last compare of the index guards nothing
            n = ((v >> 10) & 0xfff) + 1
        elif idx is not None and n is None and rd == idx and ((v >> 26) & 7 == 4 or (v >> 25) & 7 == 5):
            if (v & 0xffe0ffe0) == 0x2a0003e0:                       # mov wI, wJ: the bound is on wJ
                idx = (v >> 16) & 31
            else:
                return None                                          # written by anything else: give up
        k -= 4
    if page is None or n is None or n > 4096:
        return None
    raw = img_ro(page, 4 * n)
    if raw is None:
        return None
    return set(page + struct.unpack_from('<i', raw, 4 * i)[0] for i in range(n)), page, n


def real():
    import recomp_eh
    print('real binary %s' % recomp.ELF)
    info = recomp_eh.parse_main_eh()
    recomp_eh._eh = (info['pads'], set(info['catchers']), info['starts'])
    env = recomp.Env()
    env.imports = set()
    env.full = True
    env.starts = sorted(set(env.compute_starts()) | set(info['starts']))
    cands = [a for a in env.starts if any((recomp.word(pc) & 0xfffffc1f) == 0xd61f0000 for pc in range(a, env.extent(a), 4))]
    longest = sorted(cands, key=lambda a: env.extent(a) - a)[-40:]
    sample = cands if '--all' in sys.argv else sorted(set(cands[::8]) | set(longest))
    print('functions holding a br: %d; sample %d' % (len(cands), len(sample)))
    im = J.image(recomp)
    stat = dict(copies=0, reduced={1: 0, 2: 0}, indep=0, indep_full=0, agree=0, wider=0, tighter=0, bad=0)
    case_re = re.compile(r'^  case 0x([0-9a-f]+)ULL: goto L_([0-9a-f]+);$')

    def text(level, a):
        J.LEVEL = level
        recomp_eh.JT_STATS = []
        t = recomp_eh.lift_function(env, a)[0]
        st = recomp_eh.JT_STATS
        recomp_eh.JT_STATS = None
        return t, st
    for a in sample:
        end = env.extent(a)
        t0, _ = text(0, a)
        l0 = t0.split('\n')
        if 'L_dispatch: switch (jt) {' not in l0:                  # a PLT stub (lifted as a call of the import)
            continue
        ins = [l for l in l0 if l.startswith('L_') and not l.startswith('L_dispatch')]
        k = l0.index('L_dispatch: switch (jt) {')
        old_cases = l0[k + 1:k + 1 + len(ins)]
        ok0 = old_cases == ['  case 0x%sULL: goto L_%s;' % (l[2:l.index(':')], l[2:l.index(':')]) for l in ins] and \
            l0[k + 1 + len(ins)] == '  default: call_indirect(c, jt); return; }'
        stat['copies'] += 1
        if not ok0:
            stat['bad'] += 1
            print('FAIL  f_%x: RECOMP_JT=0 is not the old switch' % a)
            continue
        indep = {}
        for pc in range(a, end, 4):
            if (recomp.word(pc) & 0xfffffc1f) == 0xd61f0000:
                indep[pc] = independent_targets(recomp.word, im.read, pc, a, end)
        for level in (1, 2):
            t1, st = text(level, a)
            l1 = t1.split('\n')
            if [l for l in l1 if l.startswith('L_') and not l.startswith('L_dispatch')] != ins or \
                    l1[:l1.index('L_dispatch: switch (jt) {')] != l0[:k]:
                stat['bad'] += 1
                print('FAIL  f_%x level %d: the instruction lines differ from RECOMP_JT=0' % (a, level))
                continue
            k1 = l1.index('L_dispatch: switch (jt) {')
            m = k1 + 1
            cases = set()
            while case_re.match(l1[m]):
                cases.add(int(case_re.match(l1[m]).group(1), 16))
                m += 1
            full = cases == set(int(l[2:l.index(':')], 16) for l in ins) and l1[m] == '  default: call_indirect(c, jt); return; }'
            _, _, _, got, sites = st[0]
            if got is None:
                if level == 1:
                    stat['indep_full'] += sum(1 for ts in indep.values() if ts is not None)
                if not full:
                    stat['bad'] += 1
                    print('FAIL  f_%x level %d: unproven but not the full switch' % (a, level))
                continue
            stat['reduced'][level] += 1
            proven = set(t for v in sites.values() for t in v[1] if a <= t < end and t % 4 == 0) | {a}
            if cases != proven:
                stat['bad'] += 1
                print('FAIL  f_%x level %d: cases are not the proven targets + entry' % (a, level))
            if not l1[m].startswith('  default: if (jt - 0x%xULL < 0x%xULL) guest_trap(' % (a, end - a)):
                stat['bad'] += 1
                print('FAIL  f_%x level %d: default line without the in-function trap' % (a, level))
            for pc, ts in indep.items():
                if ts is None:
                    continue
                ts, page, nent = ts
                if level == 1:
                    stat['indep'] += 1
                inside = set(t for t in ts if a <= t < end)
                if not inside <= cases and sites[pc][0] == 'table' and sites[pc][2] == page and sites[pc][3] < 4 * nent:
                    # the same table, fewer entries: a tighter bound on every path (an earlier cmp, e.g.
                    # `cmp w9, #3; b.hi; cmp w9, #4; b.hi`): the entries behind it cannot be reached. Listed to look at.
                    if level == 1:
                        stat['tighter'] += 1
                        print('info  f_%x: br %x: %d of the %d entries the matcher reads are reachable (a tighter bound)'
                              % (a, pc, sites[pc][3] // 4, nent))
                elif not inside <= cases:
                    stat['bad'] += 1
                    print('FAIL  f_%x level %d: br %x: independent targets %s not cases' % (a, level, pc, ' '.join('%x' % t for t in sorted(inside - cases)[:5])))
                elif sites[pc][0] == 'table' and not ts <= set(sites[pc][1]):
                    stat['bad'] += 1
                    print('FAIL  f_%x level %d: br %x: the analysis reads fewer targets than the matcher' % (a, level, pc))
                elif level == 1:
                    stat['agree'] += 1
                    # more: another path reaches the table code with a looser bound (a join the matcher ignores)
                    stat['wider'] += sites[pc][0] == 'table' and set(sites[pc][1]) != ts
    J.LEVEL = 0
    print('lifted copies %d; reduced at level 1: %d, at level 2: %d; brs the independent matcher reads (in reduced '
          'level-1 copies): %d, covered: %d (%d of them with more table entries than the matcher reads: a looser bound on '
          'another path), %d with a tighter bound; matched brs in copies that kept the full switch: %d'
          % (stat['copies'], stat['reduced'][1], stat['reduced'][2], stat['indep'], stat['agree'], stat['wider'],
             stat['tighter'], stat['indep_full']))
    check(stat['bad'] == 0, 'real functions: %d failures' % stat['bad'])


if __name__ == '__main__':
    if REAL:
        real()
    else:
        synthetic()
    print('%d failure(s)' % len(FAILS))
    sys.exit(1 if FAILS else 0)
