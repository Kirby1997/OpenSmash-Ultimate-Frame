"""Jump tables of a lifted function: the case list of its `br` dispatch (RECOMP_JT, default 0 = off).

Without it (RECOMP_JT unset / 0) every function that holds a `br` gets the dispatch it always had:

    L_dispatch: switch (jt) { case <every instruction of the function>: goto L_<it>; default: call_indirect(c, jt); return; }

which is what made 12,166 functions hold 56 % of all lifted statements, and the code generator's cycle analysis of those
irreducible graphs ran out of memory on the biggest (build_opt.py, RECOMP_SPLIT). With RECOMP_JT=1 a function whose every
`br` is PROVEN to dispatch through a jump table of the read-only segment lists only the targets of those tables:

    L_dispatch: switch (jt) {
      case <the entry, and each table target inside the function>: goto L_<it>;
      default: if (jt - <entry> < <size>) guest_trap("...", jt); call_indirect(c, jt); return; }

SAFETY. For every value of jt this does what the old switch did, or stops: a listed target goes to its label (the old
switch went there too), a target outside the function goes to call_indirect (as before), the entry goes to L_<entry>
(as before), and any other target inside the function -- one the analysis missed, a table patched at run time -- stops
the game with GUEST TRAP and the address. It cannot jump anywhere the old code would not have. So a wrong proof costs a
loud stop, never a silent miscompile, and the proof below is there to make that stop never happen.
Any `br` that is not proven keeps the full switch for its whole function (the old code, byte for byte).
RECOMP_JT=2 also takes a `br` of a pointer loaded from memory (`ldr x / ldp x`) as a tail call: such a pointer is a
function entry (every code address the binary stores is one: relocations and adrp/add / adr pairs are all function starts,
recomp.Env.compute_starts), so it lists no target for it; the trap catches one that lands inside the function anyway.

PROOF of the targets of a `br xT`, inside the lifted copy [entry, end), from the instruction words (recomp.word: patches
applied) and the image:
  1. WINDOW: the straight run of instructions that ends at the br and starts at the first instruction that can be reached
     other than from the one before it: the entry, a landing pad, the target of a goto of the lifted text (b / b.cond /
     cbz / tbz inside the function), a target of this dispatch (fixed point, 4), or the one after a statement that does
     not fall through. Every path to the br runs the whole window from its start, in order.
  2. VALUES: the window is evaluated forward (Proof) from what holds at its start on every path (a must-dataflow over the
     lifted copy's edges: falling through, gotos, and each proven br to its own targets; nothing at the entry and the
     pads); the dataflow runs only when the window alone does not prove every br. An instruction that is not modelled
     forgets every register it may write (over-approximated: loads with writeback forget the base; svc, exclusives,
     atomics, SVE and unknown classes forget all; bl / blr forget x0..x18 and x30 -- AAPCS64, which the game's own code
     relies on wherever it keeps a value in x19..x29 across a call). Modelled: adrp / adr / add-immediate / movz / mov
     make constants; a 32-bit write zeroes the upper half; `cmp wI|xI, #N` (or `subs wD, wI, #N`) and a `b.hi` / `b.hs`
     behind it, with only instructions between that certainly leave NZCV and I alone, bound I on the not-taken path (a
     32-bit cmp bounds the low half; a zero-extended copy -- mov wD, wI / and xD, xI, #0xffffffff -- is bounded with it);
     `and #2^k-1` / ubfx bound by their width, and a cmp of one `and` bounds the same `and` made again; a register-offset
     load (ldrsw / ldr w / ldrh / ldrb ...) from a constant base with a bounded index reads the table (a read-only
     segment, no relocation in it, no site writes it, at most MAX_ENTRIES entries); `add xD, xC, xL{, lsl / extend #s}`
     of a constant and the loaded values gives the targets.
  3. the br's register must hold those targets (or a constant).
  4. their in-function targets are joins (1) and dataflow edges (2): the proof runs again until the target sets are
     stable; a br whose proof fails leaves the function unproven (full switch).
FORCE_FULL (relift_sites.py: lifted copies holding a site whose callout or run-time immediate can change registers, and
`jtable` sites) and MUTABLE (data the site list writes) keep the full switch for what the binary alone cannot prove."""
import bisect
import os
import re
import struct

# 0 (default): the full dispatch everywhere, as always. 1: proven jump tables only. 2: also tail calls through a pointer
# loaded from memory (see SAFETY above). scripts/setup.py --jumptables sets it for the lift.
LEVEL = int(os.environ.get('RECOMP_JT', '0') or 0)
MAX_ENTRIES = 4096          # a bigger bound is no jump table of compiled code
MAX_WINDOW = 64             # instructions behind a br the proof may use
FORCE_FULL = set()          # code addresses: a lifted copy that holds one keeps the full dispatch (relift_sites.py)
MUTABLE = []                # [(start, end)] data written at run time (relift_sites.py): a table touching one is not proven

GOTO = re.compile(r'goto L_([0-9a-f]+);')
TAILS = ('return;', 'goto', '{ jt', '{ f_', '{ imp_', '{ h_')     # statements that never fall through (as recomp_eh.TAILS)
M64 = (1 << 64) - 1
# AAPCS64: a call may change x0..x18 and x30; x19..x29 hold across it. The game's own code relies on that wherever it
# keeps a value in x19..x29 across a call (that is how a hoisted table base reaches the br), so the proof may too.
CALLER_SAVED = tuple(range(19)) + (30,)


class Image:
    """the read-only segments of the binary and the relocation offsets, read once per process"""
    def __init__(self, rc):
        img = rc.IMG
        phoff, = struct.unpack_from('<Q', img, 0x20)
        phnum, = struct.unpack_from('<H', img, 0x38)
        self.ro = []                                               # (guest start, guest end, file offset - guest address)
        for i in range(phnum):
            ptype, flags, off, vaddr, _, filesz, _, _ = struct.unpack_from('<IIQQQQQQ', img, phoff + i * 56)
            if ptype == 1 and flags == 4:                          # PT_LOAD, read only (not the code, not the data)
                self.ro.append((rc.BASE + vaddr, rc.BASE + vaddr + filesz, off - (rc.BASE + vaddr)))
        rel = []
        for tab, size in ((rc.RELA, rc.RELASZ), (rc.JMPREL, rc.PLTRELSZ)):
            for k in range(size // 24):
                off, = struct.unpack_from('<Q', img, rc.FILE_OFF + tab + k * 24)
                rel.append(rc.BASE + off if off < rc.BASE else off)
        self.rel = sorted(rel)
        self.img = img

    def read(self, addr, size):
        """bytes [addr, addr + size) if all are in a read-only segment, no relocation writes into them and no site
        (MUTABLE) does; else None"""
        for lo, hi, d in self.ro:
            if lo <= addr and addr + size <= hi:
                break
        else:
            return None
        k = bisect.bisect_left(self.rel, addr - 7)                 # a relocation writes up to 8 bytes at its offset
        if k < len(self.rel) and self.rel[k] < addr + size:
            return None
        for m0, m1 in MUTABLE:
            if m0 < addr + size and addr < m1:
                return None
        return self.img[addr + d:addr + d + size]


_IMAGES = {}


def image(rc):
    im = _IMAGES.get(id(rc))
    if im is None:
        im = _IMAGES[id(rc)] = Image(rc)
    return im


def _sx(v, n):
    v &= (1 << n) - 1
    return v - (1 << n) if v >> (n - 1) else v


def _ext(v, option):
    """the extend of an extended-register operand (UXTB .. SXTX) on a 64-bit value"""
    n = (8, 16, 32, 64)[option & 3]
    v &= (1 << n) - 1
    if option & 4:
        v = _sx(v, n)
    return v & M64


def _low_mask_bits(m):
    """k if m == 2^k - 1 (k >= 1), else 0"""
    return m.bit_length() if m and m & (m + 1) == 0 else 0


def _keeps_flags(w):
    """True if the instruction certainly leaves NZCV alone (a short list; anything else may set them)"""
    op0 = (w >> 25) & 0xf
    if op0 in (8, 9):
        g = (w >> 23) & 7
        return g in (0, 1, 5, 6, 7) or (g == 2 and not (w >> 29) & 1) or (g == 4 and (w >> 29) & 3 != 3)
    if op0 in (5, 13):
        if (w & 0x1f000000) == 0x0a000000:
            return (w >> 29) & 3 != 3                              # not ands / bics
        if (w & 0x1f000000) == 0x0b000000 or (w & 0x1fe00000) == 0x1a000000:
            return not (w >> 29) & 1                               # add / sub / adc / sbc without S
        return (w & 0x1fe00000) in (0x1a800000, 0x1ac00000) or (w & 0x1f000000) == 0x1b000000
    if op0 == 7:
        return True                                                # Advanced SIMD (bit 28 = 0): no NZCV writer there
    if op0 & 5 == 4:
        return (w & 0x3f000000) in (0x39000000, 0x3d000000, 0x29000000, 0x2d000000) or \
            (w & 0x3b200000) == 0x38000000 or (w & 0x3b200c00) == 0x38200800 or (w & 0x3a000000) == 0x28000000
    return False


class Proof:
    """forward evaluation of one window. State per register (31 never tracked):
    ('C', v) constant; ('Z',) written by a 32-bit write in the window (upper half 0); ('B', n) in [0, n);
    ('L', [v...]) the table entries a bounded load read (as the register holds them); ('T', [t...]) branch targets."""
    def __init__(self, rc, im):
        self.rc, self.im = rc, im
        self.st = {}
        self.pending = None           # (address of the next instruction, register, sf, imm) after a `cmp #imm`
        self.table = None             # (base, size in bytes) of the last table read

    def _written(self, r):
        """r is written: a copy ('W', r) of its old low half is still zero-extended, ('A', r, k) still below 2^k, but
        neither is a function of r any more"""
        for q, v in list(self.st.items()):
            if type(q) is tuple:                                   # ('k', value): a bound known for a value of r
                if q[1][1] == r:
                    del self.st[q]
            elif v[0] in ('W', 'A') and v[1] == r:
                self.st[q] = ('Z',) if v[0] == 'W' else ('B', 1 << v[2])

    def put(self, r, v):
        if r != 31:
            self._written(r)
            if v is None:
                self.st.pop(r, None)
            else:
                self.st[r] = v

    def forget(self, regs):
        if regs is None:
            self.st.clear()
        else:
            for r in regs:
                self.put(r, None)

    def get(self, r, zr):
        if r == 31:
            return ('C', 0) if zr else None
        return self.st.get(r)

    def step(self, w, pc):
        pend = self.pending
        if pend and pend[0] == pc and (w & 0xff000010) != 0x54000000 and _keeps_flags(w):
            r = pend[1]                                            # cmp; <no flags, r not written>; b.hi: still the cmp's flags
            probe = Proof(self.rc, self.im)
            probe.st = {k: ('S', k) for k in range(31)}
            probe._step(w, pc)
            self._step(w, pc)
            if probe.st.get(r) == ('S', r):
                self.pending = (pc + 4,) + pend[1:]
            return
        self._step(w, pc)

    def _step(self, w, pc):
        rc = self.rc
        pend, self.pending = self.pending, None
        rd, rn = w & 31, (w >> 5) & 31
        sf = w >> 31
        op0 = (w >> 25) & 0xf
        if op0 in (8, 9):                                          # data processing, immediate
            g = (w >> 23) & 7
            if g in (0, 1):                                        # adr / adrp
                imm = _sx((((w >> 5) & 0x7ffff) << 2) | ((w >> 29) & 3), 21)
                self.put(rd, ('C', (((pc & ~0xfff) + (imm << 12)) if sf else pc + imm) & M64))
            elif g == 2:                                           # add / sub (immediate)
                imm = ((w >> 10) & 0xfff) << (12 if (w >> 22) & 1 else 0)
                sub, s = (w >> 30) & 1, (w >> 29) & 1
                if s and rd == 31:                                 # cmp / cmn: flags only
                    if sub:
                        self.pending = (pc + 4, rn, sf, imm)
                    return
                a = self.get(rn, False)                            # rn 31 = sp
                if not s and sf and a and a[0] == 'C':
                    self.put(rd, ('C', (a[1] - imm if sub else a[1] + imm) & M64))
                elif not s and rd == 31:
                    pass                                           # writes sp
                else:
                    self.put(rd, None if sf else ('Z',))
                    if s and sub and rd != rn and rn != 31:
                        self.pending = (pc + 4, rn, sf, imm)       # subs wd, wn, #imm: the flags of cmp wn, #imm
            elif g == 4:                                           # logical (immediate)
                opc = (w >> 29) & 3
                size = 64 if sf else 32
                try:
                    mask, _ = rc.decode_bitmasks((w >> 22) & 1, (w >> 10) & 0x3f, (w >> 16) & 0x3f, size)
                except rc.Unimpl:
                    self.put(rd, None)
                    return
                if opc == 3 and rd == 31:                          # tst
                    return
                if opc in (0, 1, 2) and rd == 31:                  # writes sp
                    return
                k = _low_mask_bits(mask)
                a = self.get(rn, True)
                if opc in (0, 3) and k and a and a[0] in ('B', 'B32') and (a[0] == 'B' or k <= 32):
                    self.put(rd, ('B', min(a[1], 1 << k)))         # the low k bits of a bounded value
                elif opc in (0, 3) and k and k <= 16:
                    # xn & (2^k - 1): bounded; and while xn is not written, a cmp of another register holding the same
                    # value (`and w9, w2, #0xff; cmp w9, #7; b.hi; and x9, x2, #0xff`) bounds this one too
                    src = rn if not (a and a[0] == 'W') else a[1]  # (low half of xm) & m = xm & m, m < 2^32
                    sym = ('A', src, k)
                    self.put(rd, self.st.get(('k', sym), sym) if src not in (rd, 31) else ('B', 1 << k))
                elif opc == 0 and k == 32 and rn != rd and rn != 31:
                    self.put(rd, self.st.get(('k', ('W', rn)), ('W', rn)))       # and xd, xn, #0xffffffff = mov wd, wn
                elif opc == 1 and rn == 31:
                    self.put(rd, ('C', mask))
                else:
                    self.put(rd, None if sf else ('Z',))
            elif g == 5:                                           # move wide
                opc, hw, imm = (w >> 29) & 3, ((w >> 21) & 3) * 16, (w >> 5) & 0xffff
                if opc == 2:
                    self.put(rd, ('C', imm << hw))
                else:
                    self.put(rd, None if sf else ('Z',))
            elif g == 6:                                           # bitfield
                opc, immr, imms = (w >> 29) & 3, (w >> 16) & 0x3f, (w >> 10) & 0x3f
                width = imms - immr + 1
                if opc == 2 and imms >= immr and width <= 16:      # ubfx / lsr / uxtb / uxth
                    self.put(rd, ('B', 1 << width))
                else:
                    self.put(rd, None if sf else ('Z',))
            else:                                                  # extract, unallocated
                self.put(rd, None)
            return
        if op0 in (5, 13):                                         # data processing, register
            if (w & 0x7f200000) == 0x0b000000 or (w & 0x7fe00000) == 0x0b200000:     # add (shifted / extended)
                if sf:
                    self.add64(w, rd, rn)
                elif rd != 31 or not (w >> 21) & 1:
                    self.put(rd, ('Z',))
                return
            if (w & 0x1f000000) == 0x0b000000:                     # sub / adds / subs (shifted, extended)
                if not (w >> 29) & 1 or rd != 31:                  # cmp / cmn write zr
                    if (w >> 21) & 1 and not (w >> 29) & 1 and rd == 31:
                        return                                     # extended form, rd = sp
                    self.put(rd, None if sf else ('Z',))
                return
            if (w & 0x1f000000) == 0x0a000000:                     # logical (shifted register)
                if (w & 0xffe0ffe0) == 0xaa0003e0:                 # mov xd, xm
                    self.put(rd, self.get((w >> 16) & 31, True))
                    return
                if (w & 0xffe0ffe0) == 0x2a0003e0:                 # mov wd, wm: xd = the low half of xm, zero-extended
                    rm = (w >> 16) & 31
                    v = self.get(rm, True)
                    if v and _upper0(v):
                        self.put(rd, v)
                    elif v and v[0] == 'B32':
                        self.put(rd, ('B', v[1]))
                    elif rm != rd and rm != 31:
                        self.put(rd, self.st.get(('k', ('W', rm)), ('W', rm)))   # bounded once a cmp of wm bounds it
                    else:
                        self.put(rd, ('Z',))
                    return
                self.put(rd, None if sf else ('Z',))
                return
            if (w & 0x1fe00000) in (0x1a000000, 0x1a800000, 0x1ac00000) or (w & 0x1f000000) == 0x1b000000:
                self.put(rd, None if sf else ('Z',))               # adc / sbc, csel family, 1-/2-source, 3-source
                return
            self.put(rd, None)                                     # ccmp (no write) and the rest: forget rd
            return
        if op0 & 5 == 4:                                           # loads and stores
            self.loadstore(w, rd, rn)
            return
        if op0 in (10, 11):                                        # branches, exceptions, system
            if (w & 0x7c000000) == 0x14000000:                     # b / bl
                if w >> 31:
                    self.forget(CALLER_SAVED)
                return
            if (w & 0xff000010) == 0x54000000:                     # b.cond
                cond = w & 0xf
                if pend and pend[0] == pc and cond in (8, 2):      # cmp #imm; b.hi / b.hs: not taken -> bounded
                    _, r, csf, imm = pend
                    n = imm + 1 if cond == 8 else imm
                    v = self.get(r, False)
                    if r != 31:
                        if v and _bound(v):
                            n = min(n, _bound(v))
                        elif v and v[0] == 'B32':
                            n = min(n, v[1])
                        for q, u in list(self.st.items()):         # copies of wr, and registers holding the same value
                            if type(q) is int and q != r and (u == ('W', r) or (v and v[0] in ('A', 'W') and u == v)):
                                self.st[q] = ('B', n)
                        if v and v[0] in ('A', 'W'):
                            self.st[('k', v)] = ('B', n)           # and for that value made again later
                        self.st[('k', ('W', r))] = ('B', n)        # the low half of xr is below n (either cmp width)
                        if csf or (v and _upper0(v)):
                            self.st[r] = ('B', n)                  # the whole register
                        else:
                            self.st[r] = ('B32', n)                # its low half only (a 32-bit cmp, upper half unknown)
                return
            if (w & 0x7e000000) in (0x34000000, 0x36000000):       # cbz / cbnz / tbz / tbnz
                return
            if (w & 0xfffffc1f) == 0xd63f0000:                     # blr
                self.forget(CALLER_SAVED)
                return
            if (w & 0xfffffc1f) == 0xd61f0000:                     # br writes no register (its cases see what it saw)
                return
            if (w & 0xfe000000) == 0xd6000000:                     # br / ret / pointer-auth forms
                self.forget(None)
                return
            if (w & 0xffc00000) == 0xd5000000:                     # system: hints, barriers, msr, mrs, sys(l)
                if (w >> 21) & 1:
                    self.put(rd, None)                             # mrs / sysl write rt
                return
            self.forget(None)                                      # svc / hvc / brk ...
            return
        if op0 in (7, 15):                                         # FP / SIMD: only three groups write a GPR (at bits 4..0)
            if (w & 0x7f20fc00) == 0x1e200000 or (w & 0x7f200000) == 0x1e000000 or (w & 0x9fe08400) == 0x0e000400:
                self.put(rd, None)                                 # FP <-> integer conversions / fmov; FP <-> fixed-point
                                                                   # (fcvtzs / fcvtzu xd, dn, #fbits); umov / smov / dup / ins
            return
        self.forget(None)                                          # SVE, SME, unallocated

    def add64(self, w, rd, rn):
        rm = (w >> 16) & 31
        s = (w >> 29) & 1
        if (w >> 21) & 1:                                          # extended register: rn may be sp, rm is extended
            option, sh = (w >> 13) & 7, (w >> 10) & 7
            a, b = self.get(rn, False), self.get(rm, True)
            f = (lambda v: (_ext(v, option) << sh) & M64) if sh <= 4 else None
            if s and rd == 31:
                return
            if s or rd == 31 or f is None:
                self.put(rd, None)
                return
        else:
            kind, sh = (w >> 22) & 3, (w >> 10) & 0x3f
            a, b = self.get(rn, True), self.get(rm, True)
            if s and rd == 31:
                return
            if kind or s:
                self.put(rd, None)
                return
            f = lambda v: (v << sh) & M64
        vals = []                                                  # a + f(b): one side a constant, the other the table
        for x in (a, b):
            if not x or x[0] not in ('C', 'L'):
                self.put(rd, None)
                return
            vals.append([x[1]] if x[0] == 'C' else x[1])
        if a[0] == 'L' and b[0] == 'L':
            self.put(rd, None)
            return
        out = [(p + f(q)) & M64 for p in vals[0] for q in vals[1]]
        self.put(rd, ('C', out[0]) if a[0] == b[0] == 'C' else ('T', out))

    def loadstore(self, w, rt, rn):
        v = (w >> 26) & 1
        if (w & 0x3b200c00) == 0x38200800:                         # register offset (no writeback)
            size, opc = w >> 30, (w >> 22) & 3
            if v or opc == 0 or (size == 3 and opc == 2):          # SIMD / store / prfm: no GPR written
                return
            if size == 2 and opc == 3:
                self.put(rt, None)
                return
            rm, option, S = (w >> 16) & 31, (w >> 13) & 7, (w >> 12) & 1
            base, idx = self.get(rn, False), self.get(rm, False)
            n = (_bound(idx) if option in (2, 3, 6) else None) if idx else None
            if idx and idx[0] == 'B32' and option in (2, 6):     # uxtw / sxtw use the low half only
                n = idx[1]
            if base and base[0] == 'C' and n and n <= MAX_ENTRIES:
                vals = self.read_table(base[1], n, size, S, opc)
                if vals is not None:
                    self.put(rt, ('L', vals))
                    return
            self.put(rt, self.load_dest(size, opc))
            return
        if (w & 0x3b000000) == 0x39000000:                         # unsigned offset (no writeback)
            size, opc = w >> 30, (w >> 22) & 3
            if not v and opc and not (size == 3 and opc == 2):
                self.put(rt, self.load_dest(size, opc))
            return
        if (w & 0x3b200000) == 0x38000000:                         # unscaled / unprivileged / pre- / post-index
            size, opc, mode = w >> 30, (w >> 22) & 3, (w >> 10) & 3
            if not v and opc and not (size == 3 and opc == 2):
                self.put(rt, self.load_dest(size, opc) if mode in (0, 2) else None)
            if mode in (1, 3):
                self.put(rn, None)
            return
        if (w & 0x3a000000) == 0x28000000:                         # pairs
            if not v and (w >> 22) & 1:
                m = ('M',) if w >> 30 == 2 else None              # ldp x: pointers from memory
                self.put(rt, m)
                self.put((w >> 10) & 31, m)
            if (w >> 23) & 3 in (1, 3):
                self.put(rn, None)
            return
        self.forget(None)                                          # exclusives, atomics, literal, SIMD structures ...: all

    @staticmethod
    def load_dest(size, opc, pointer=True):
        """what a load of (size, opc) leaves in rt that is known without its value"""
        if (opc == 1 and size < 3) or (opc == 3 and size < 2):
            return ('Z',)                                          # ldr w / ldrb / ldrh, ldrsb w / ldrsh w
        if pointer and size == 3 and opc == 1:
            return ('M',)                                          # ldr x from [xn, #imm]: a pointer from memory
        return None

    def read_table(self, base, n, size, S, opc):
        scale = size if S else 0
        esize = 1 << size
        span = ((n - 1) << scale) + esize
        raw = self.im.read(base, span)
        if raw is None:
            return None
        out = []
        for i in range(n):
            o = i << scale
            e = int.from_bytes(raw[o:o + esize], 'little')
            if opc == 2:
                e = _sx(e, 8 * esize) & M64                        # ldrsb / ldrsh / ldrsw x
            elif opc == 3:
                e = _sx(e, 8 * esize) & 0xffffffff                 # ldrsb / ldrsh w
            out.append(e)
        self.table = (base, span)
        return out


def _br_sites(rc, lines):
    return [pc for pc, _ in lines if (rc.word(pc) & 0xfffffc1f) == 0xd61f0000]


def _upper0(v):
    return v[0] in ('Z', 'B', 'W', 'A') or (v[0] == 'C' and v[1] < 1 << 32)


def _bound(v):
    """n if the register is known to be in [0, n), else None"""
    return v[1] if v[0] == 'B' else (1 << v[2]) if v[0] == 'A' else None


def _meet(a, b):
    out = {}
    for r, v in a.items():
        w = b.get(r)
        if w is None:
            continue
        if v == w:
            out[r] = v
        elif v[0] == w[0] == 'B32':
            out[r] = ('B32', max(v[1], w[1]))
        elif _bound(v) and _bound(w):
            out[r] = ('B', max(_bound(v), _bound(w)))
        elif _upper0(v) and _upper0(w):
            out[r] = ('Z',)
    return out


def _jumps_to(stmt, t):
    return ('goto L_%x;' % t) in stmt


def _dataflow(rc, im, pcs, stmt, tops, edges):
    """what holds in each register at the start of each instruction on every path (a must-analysis; missing = nothing
    known). tops: the instructions entered from nowhere known (entry, landing pads): nothing known there. Edges: falling
    through (statements not in TAILS), the gotos of the lifted text, and edges = {br: its proven in-function targets}.
    The per-br edges assume each table is what the binary holds; if it is not at run time, the br's target is still
    either a case (where the old full switch went too), outside the function (call_indirect, as before) or a trap."""
    idx = {pc: i for i, pc in enumerate(pcs)}
    n = len(pcs)
    succ = []
    for i, pc in enumerate(pcs):
        s = stmt[pc]
        out = [] if s.startswith(TAILS) or i + 1 == n else [i + 1]
        if 'goto L_' in s:
            out.extend(idx[t] for t in (int(m, 16) for m in GOTO.findall(s)) if t in idx)
        succ.append(out)
    for b, ts in edges.items():
        succ[idx[b]].extend(idx[t] for t in ts if t in idx)
    top = set(idx[t] for t in tops if t in idx)
    npred = [0] * n
    for i, out in enumerate(succ):
        for j in out:
            npred[j] += 1
    # a cmp's flags reach the b.cond behind it along fall-through instructions that are entered from nothing else
    pend_in = [None] * n
    p = Proof(rc, im)
    for i in range(1, n):
        if npred[i] == 1 and i not in top and i in succ[i - 1] and pcs[i] == pcs[i - 1] + 4:
            p.st, p.pending = {}, pend_in[i - 1]
            p.step(rc.word(pcs[i - 1]), pcs[i - 1])
            pend_in[i] = p.pending
    state = [None] * n
    for i in top:
        state[i] = {}
    work = sorted(top, reverse=True)
    while work:
        i = work.pop()
        w = rc.word(pcs[i])
        p.st, p.pending = dict(state[i]), pend_in[i]
        p.step(w, pcs[i])
        o = o_jump = p.st
        if pend_in[i] is not None and (w & 0xff000010) == 0x54000000:
            # cmp; b.hi: the bound holds on the fall-through edge only; the branch target gets the state without it
            p.st, p.pending = dict(state[i]), None
            p.step(w, pcs[i])
            o_jump = p.st
        for j in succ[i]:
            if j in top:
                continue
            oj = o if j == i + 1 and succ[i].count(j) == 1 and not _jumps_to(stmt[pcs[i]], pcs[j]) else o_jump
            new = dict(oj) if state[j] is None else _meet(state[j], oj)
            if new != state[j]:
                state[j] = new
                work.append(j)
    return {pc: (state[i] or {}) for i, pc in enumerate(pcs)}


def analyze(rc, entry, end, lines, pads=(), level=None):
    """(cases, sites): cases = the sorted in-function targets of every br of the lifted copy (+ the entry), or None
    (keep the full dispatch); sites = {br: (kind, targets, table base, table bytes)}, kind 'table' (proven), 'tail'
    (level 2: a pointer loaded from memory, i.e. a tail call; no in-function target is expected) or None (unproven)."""
    level = LEVEL if level is None else level
    im = image(rc)
    brs = _br_sites(rc, lines)
    if not brs:
        return None, {}
    if FORCE_FULL and any(entry <= a < end for a in FORCE_FULL):
        return None, {b: (None, (), 0, 0) for b in brs}
    stmt = dict(lines)
    pcs = [pc for pc, _ in lines]
    tops = {entry} | {p for p in pads if entry <= p < end}
    base_joins = set(tops)
    for _, s in lines:
        if 'goto L_' in s:
            for m in GOTO.findall(s):
                t = int(m, 16)
                if entry <= t < end:
                    base_joins.add(t)
    edges, flow = {}, None                 # edges: br -> its in-function targets, as proven in the round before
    while True:
        joins = base_joins.union(*edges.values()) if edges else base_joins
        sites, new_edges = {}, {}
        for b in brs:
            a, k = b, 0
            while a > entry and a not in joins and k < MAX_WINDOW and not stmt.get(a - 4, 'return;').startswith(TAILS):
                a -= 4
                k += 1
            p = Proof(rc, im)
            if flow is not None:
                p.st = dict(flow[a])
            while a < b:
                p.step(rc.word(a), a)
                a += 4
            v = p.st.get((rc.word(b) >> 5) & 31)
            if v and v[0] in ('T', 'C'):
                ts = v[1] if v[0] == 'T' else [v[1]]
                sites[b] = ('table', ts, p.table[0] if p.table else 0, p.table[1] if p.table else 0)
                new_edges[b] = frozenset(t for t in ts if entry <= t < end and t % 4 == 0)
            elif v and v[0] == 'M' and level >= 2:
                sites[b] = ('tail', (), 0, 0)
            else:
                sites[b] = (None, (), 0, 0)
        if any(s[0] is None for s in sites.values()) and flow is not None:
            return None, sites
        if new_edges == edges and flow is not None:
            return sorted(set().union(*edges.values()) | {entry}), sites
        if new_edges == edges and not any(s[0] is None for s in sites.values()):
            return sorted(set().union(*edges.values()) | {entry}), sites       # the windows alone (no flow needed)
        edges = new_edges
        if flow is not None or any(s[0] is None for s in sites.values()):
            flow = _dataflow(rc, im, pcs, stmt, tops, edges)


def dispatch(rc, entry, end, lines, pads=(), stats=None):
    """the L_dispatch block of a lifted copy (lines = [(pc, statement)]); with RECOMP_JT off the full switch, as always"""
    cases = None
    if LEVEL:
        cases, sites = analyze(rc, entry, end, lines, pads)
        if stats is not None:
            stats.append((entry, end, len(lines), cases, sites))
    if cases is None:
        out = ['L_dispatch: switch (jt) {']
        out.extend('  case 0x%xULL: goto L_%x;' % (pc, pc) for pc, _ in lines)
        out.append('  default: call_indirect(c, jt); return; }')
        return out
    out = ['L_dispatch: switch (jt) {']
    out.extend('  case 0x%xULL: goto L_%x;' % (t, t) for t in cases)
    out.append('  default: if (jt - 0x%xULL < 0x%xULL) guest_trap("br: a target inside f_%x that is in none of its jump '
               'tables (RECOMP_JT=0 lifts the full dispatch)", jt); call_indirect(c, jt); return; }'
               % (entry, end - entry, entry))
    return out
