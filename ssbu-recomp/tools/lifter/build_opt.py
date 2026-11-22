"""Optimised, side-by-side build of the lifted code (phase P3, plan\\phases\\P3_performance.md).

    python build_opt.py main [jobs]              gen_eh_o2\\*.c -> obj_eh_o2\\*.obj -> ssbu_game_eh_o2.dll (+ .lib)
    python build_opt.py nro [jobs] [module ...]  lift (if needed) + compile + link the script modules into nro_bin_eh_o2\\
    python build_opt.py one <file.c> [limit]     compile one file of the main set, print time and the fallback used
    python build_opt.py sizes <dir>              the largest functions of a generated folder

It does not replace build.py / build_nro.py: it imports them and swaps only the function that runs the compiler,
so dependency checks, linking and names stay theirs. Folders and names come from the same environment variables
(defaults here: RECOMP_GEN=gen_eh_o2 RECOMP_OBJ=obj_eh_o2 RECOMP_DLL=ssbu_game_eh_o2.dll RECOMP_NRO_SUFFIX=_eh_o2).
Run the result with:  RECOMP_GEN=gen_eh_o2 RECOMP_OBJ=obj_eh_o2 RECOMP_DLL=ssbu_game_eh_o2.dll RECOMP_EXE=<exe> python build.py link
                      SSBU_NRO_BIN=nro_bin_eh_o2 <exe> ...

Why a file can take hours at -O1/-O2: a few lifted functions have tens of thousands of instructions (the largest
215,493) in one C function with thousands of labels; several LLVM passes are worse than linear in that. Rule used
here, automatic and bounded in time:
  1. every function longer than OPTNONE_LINES statements is compiled unoptimised (`optnone`), the rest of its file at
     the chosen level; the attribute is given through a generated header (-include), the generated C is untouched
     (linux-arm64, SPLIT: those functions are compiled at -O0 in a translation unit of their own instead, joined to the
     rest with ld.lld -r - see SPLIT below);
  2. if a file still exceeds TIME_LIMIT seconds, it is retried with the limit divided by 4, then by 16;
  3. if that still exceeds the time limit, the whole file is compiled at -O0.
Every fallback is printed and written to <obj dir>\\_opt_report.txt.

Flags: -fno-strict-aliasing as in the -O0 build; -ffp-contract=off (no fused multiply-add can be formed: the -O0 build
never formed one either; fused operations the game asks for are explicit fma() calls); no fast-math; the default x86-64
target (SSE2), so no FMA / AVX instructions are selected.
"""
import collections
import hashlib
import json
import os
import re
import subprocess
import signal
import sys
import threading
import time

os.environ.setdefault('RECOMP_GEN', 'gen_eh_o2')
os.environ.setdefault('RECOMP_OBJ', 'obj_eh_o1')          # P3 (2026-10-09): the set that is the default of build.py
os.environ.setdefault('RECOMP_DLL', 'ssbu_game_eh_o1.dll')
os.environ.setdefault('RECOMP_NRO_SUFFIX', '_eh_o2')

import build          # noqa: E402  (reads the environment at import)
import build_nro      # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
CLANG = os.environ.get('RECOMP_CLANG', r'C:\llvm-9.0.0\bin\clang.exe')
# (getattr: tests/build_opt_sched_test.py imports this module with an empty stand-in for build)
TARGET_ARM64 = getattr(build, 'TARGET_ARM64', False)
OPT = os.environ.get('RECOMP_OPT', '-O2' if TARGET_ARM64 else '-O1')
if TARGET_ARM64 and OPT not in ('-O2', '-O3'):
    print('RECOMP_OPT=%s: the Frame build is tuned for -O2 or better; every step of the ladder below it is reported in _opt_report.txt' % OPT)
EXTRA = ['-ffp-contract=off'] + getattr(build, 'CODEGEN_FLAGS', []) + os.environ.get('RECOMP_CFLAGS', '').split()   # CODEGEN_FLAGS = the aarch64 driver flags (empty on windows / x86-64 linux)
# RECOMP_INLINE=1: the helpers of runtime.h / neon.h / fpexact.h (every guest load / store, flag, NEON op) inlined into the
# lifted code. clang 9 at -O1 runs only the always-inline inliner, so without this each helper is a real call: measured
# 2026-10-13 (private round 15): main thread -25-30 %, audio update 9-17 -> 2-4 ms, per-frame hash identical. Same
# sources, same floating-point flags; a file whose inlined compile fails is compiled again without it (compile_file).
INLINE_FLAG = '-Dinline=__inline__ __attribute__((always_inline))'
INLINE = os.environ.get('RECOMP_INLINE', '0') not in ('', '0')      # the game code only (cmd_main); the script modules keep EXTRA
INLINE_LIMIT = float(os.environ.get('RECOMP_INLINE_LIMIT', '20'))   # seconds an inlined compile may take; longer: the file is compiled without
# linux-arm64 at -O2 / -O3: the inlined compile is the file's only compile, with the normal ladder (no separate 20 s try).
# There the -O2 inliner inlines the helpers by itself, so with or without INLINE_FLAG the compile takes the same time and
# makes nearly the same code (measured 2026-11-21, LLVM 19 cross: c_04cd.c 19.3 / 19.1 s, 27 of 31 functions identical and
# the same 362 calls, none to a helper; c_04ce.c 48 / 53 s): the 20 s try only cost time on the files that need longer
# (the Frame's first build: 376 files went over it, ~2 compiler-hours spent on tries that were thrown away).
INLINE_ONE_TRY = INLINE and TARGET_ARM64 and OPT in ('-O2', '-O3') and os.environ.get('RECOMP_INLINE_ONE_TRY', '1') not in ('', '0')
OPTNONE_LINES = int(os.environ.get('RECOMP_OPTNONE_LINES', '20000'))
TIME_LIMIT = float(os.environ.get('RECOMP_TIME_LIMIT', '180'))
# linux-arm64 (RECOMP_SPLIT, default on there): a function over the ladder's statement limit is not marked `optnone` in its
# own file but compiled in a translation unit of its own (split_source) at SPLIT_OPT, the rest of the file at OPT, and the
# two objects are joined into the file's one object (ld.lld -r). The long function gets the code optnone gave it (-O0; it
# was noinline already, so nothing could be inlined from or into it either way; helpers as calls, see split_finish), same
# flags otherwise (-ffp-contract=off ...).
# Why: nearly every such function dispatches its `br` through a switch with a case for EVERY instruction (recomp_eh.py,
# L_dispatch). At -O2, `optnone` skips the optimisation passes but not the analyses the -O2 code generator asks for:
# MachineCycleInfo on that irreducible graph ran out of memory (measured 2026-11-21, LLVM 19 cross to aarch64, c_03d9.c, one
# function of 77,061 statements: over 9 GB in llc's "Machine Cycle Info Analysis"; the Frame's first build: 257 s and an
# 11.3 GB reservation). Alone at -O0 the same function takes 14 s and 1.5 GB, the rest of the file 1.5 s at -O2; c_00a3.c
# and c_013a.c, which fell back to WHOLE FILE -O0, take 2 + 39 s and 4 + 17 s that way, the rest of each fully at -O2.
SPLIT_WANTED = TARGET_ARM64 and os.environ.get('RECOMP_SPLIT', '1') not in ('', '0')
SPLIT = False          # set by cmd_main / `one` (the game's own files, whose functions funcs.h declares); the script modules: optnone as before
SPLIT_OPT = os.environ.get('RECOMP_SPLIT_OPT', '-O0')
LLD = os.environ.get('RECOMP_LLD') or (os.path.join(os.path.dirname(CLANG), 'ld.lld') if os.path.exists(os.path.join(os.path.dirname(CLANG), 'ld.lld')) else 'ld.lld')
# Linux build: address-space limit per compiler, bytes (0: none). Set by limit_memory(): the most one compiler may get.
MEM_LIMIT = 0
# Linux and macOS builds (RECOMP_TARGET linux / linux-arm64 / macos): the RAM all compilers share, bytes (0: no sharing, as on Windows).
# Each compile reserves an estimate of what it needs before it starts (mem_estimate) and gives it back when it ends, so
# the job count can follow the CPU threads while the few files that need many GB run with fewer beside them. On Linux the
# reservation is also the compiler's address-space limit: a file that passes it is compiled again with twice as much (up
# to MEM_BUDGET) before it takes the next step of the fallback ladder. macOS enforces no limit: there it only paces.
MEM_BUDGET = 0
MEM_KEY = [0]                            # the budget's part of the _ladder.json key: RAM-based, not what was free at the start
# A compile waiting for its share of RAM checks the queue this often, and prints one line each time it has waited this long
# (RECOMP_MEM_WAIT_WARN, seconds; 0: never): a step that stops making progress then says so in logs/compile.log instead of
# going quiet, which is what made the 2026-10-22 report (issue 75) take days to read.
MEM_WAIT_TICK = 5.0
MEM_WAIT_WARN = float(os.environ.get('RECOMP_MEM_WAIT_WARN', '120'))
POSIX_TARGET = os.environ.get('RECOMP_TARGET') in ('linux', 'linux-arm64', 'macos')
DEF = re.compile(r'^(?:static )?void (\w+)\((Ctx \*c(?:, uint64_t rp)?)\) \{$')


def function_sizes(path):
    """[(statements, name, parameters)] for every function defined in a generated C file."""
    out = []
    cur = None
    n = 0
    with open(path, encoding='latin-1') as f:
        for line in f:
            if cur is None:
                m = DEF.match(line.rstrip('\n'))
                if m:
                    cur = (m.group(1), m.group(2))
                    n = 0
            elif line.startswith('}'):
                out.append((n, cur[0], cur[1]))
                cur = None
            else:
                n += 1
    return out


def optnone_header(src, obj, limit):
    """Write <obj>.optnone.h naming the functions of src longer than `limit` statements; returns (path, count, statements)."""
    big = [(n, name, params) for n, name, params in function_sizes(src) if n > limit]
    path = obj[:-4] + '.optnone.h'
    if not big:
        if os.path.exists(path):
            os.remove(path)
        return None, 0, 0
    with open(path, 'w') as f:
        f.write('/* generated by build_opt.py: functions too long to optimise in bounded time */\n#include "runtime.h"\n')
        for n, name, params in big:
            f.write('void %s(%s) __attribute__((optnone, noinline));\n' % (name, params))
    return path, len(big), sum(b[0] for b in big)


def split_source(src, obj, limit):
    """SPLIT: write <obj>.split_a.c (src without its functions longer than `limit` statements) and <obj>.split_b.c (those
    functions only), both with src's own lines before its first function (the comment and the #includes; every function
    is declared in funcs.h, and a generated file has nothing else at file scope: one-line wrappers such as
    `void f_X(Ctx *c) { u_X(c, 0); }` stay in part a). Part a also declares every function moved to part b: recomp_eh.py
    leaves a u_X out of funcs.h when all its landing pads belong to an outer function, and its f_X wrapper (part a) calls
    it. Returns (a, b, count, statements); (None, None, 0, 0) when no function is that long (nothing written; old parts
    removed); (None, None, -1, 0) when a long function is `static` (part a could not call it: optnone instead)."""
    base = obj[:-4]
    head, a, b = [], [], []
    big, protos = [], []
    cur = None
    static = False
    with open(src, encoding='latin-1') as f:
        for line in f:
            if cur is None:
                m = DEF.match(line.rstrip('\n'))
                if m:
                    cur, n, proto = [line], 0, 'void %s(%s);\n' % (m.group(1), m.group(2))
                elif not a and not b and (line.startswith('#') or line.startswith('/*')):
                    head.append(line)
                else:
                    a.append(line)
            else:
                cur.append(line)
                if line.startswith('}'):
                    if n > limit:
                        b.extend(cur)
                        big.append(n)
                        protos.append(proto)
                        static = static or cur[0].startswith('static ')
                    else:
                        a.extend(cur)
                    cur = None
                else:
                    n += 1
    if cur is not None:                  # (an unterminated function: not a generated file; compiled as it is)
        big = []
    paths = (base + '.split_a.c', base + '.split_b.c')
    if not big or static:
        split_clean(obj)
        return None, None, -1 if big else 0, 0
    for p, body in zip(paths, (protos + a, b)):
        with open(p, 'w', encoding='latin-1') as f:
            f.writelines(head + body)
    return paths[0], paths[1], len(big), sum(big)


def split_clean(obj):
    """Remove SPLIT's intermediate files of obj (the parts' sources and objects)."""
    base = obj[:-4]
    for p in (base + '.split_a.c', base + '.split_b.c', base + '.part_a.o', base + '.part_b.o'):
        if os.path.exists(p):
            os.remove(p)


def split_finish(b_src, obj, extra, base_flags):
    """SPLIT, after part a compiled to <obj>.part_a.o: compile part b (the long functions) at SPLIT_OPT, without a time limit
    (the -O0 code generator is linear; the whole-file -O0 fallback had none either), and join both into obj with ld.lld -r.
    Part b never gets the inlined helpers (INLINE_FLAG is taken out of `extra`): always_inline is honoured at -O0, and
    there it tripled the code of these functions, which are not hot (Frame, clang 21: c_00a3.c's object 8.6 -> 26.9 MB of
    text, the game library +27 MB, with range-extension thunks; LLVM 19: c_03d9.c's part 14 -> 50 s, 1.5 -> 2.9 GB).
    Returns (exit code, output, seconds)."""
    base = obj[:-4]
    pa, pb = base + '.part_a.o', base + '.part_b.o'
    cmd = [CLANG, '-c', SPLIT_OPT] + [f for f in extra if f != INLINE_FLAG] + base_flags + [b_src, '-o', pb]
    want = mem_split_estimate(b_src) if MEM_BUDGET else 0
    t = time.time()
    while True:
        if want:
            want = mem_acquire(want)
        try:
            code, out, _ = run_limited(cmd, None, want)
        finally:
            if want:
                mem_release(want)
        if code is None and out == OOM and want and want < MEM_BUDGET:
            want = min(MEM_BUDGET, want * 2)
            continue
        break
    if code is None and out == OOM:      # over the whole budget (a game running at the start): no address-space limit, as
        want = mem_acquire(MEM_BUDGET)   # the whole-file -O0 fallback has none; alone, with the whole budget
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, errors='replace')
        finally:
            mem_release(want)
        code, out = r.returncode, (r.stdout + r.stderr)[-2000:]
    if code == 0:
        r = subprocess.run([LLD, '-r', pa, pb, '-o', obj], capture_output=True, text=True, errors='replace')
        code, out = r.returncode, (out + r.stdout + r.stderr)[-2000:]
    elif code is None:
        code = 1
    for p in (pa, pb) + ((base + '.split_a.c', b_src) if code == 0 else ()):     # (a failed part's sources stay, to look at)
        if os.path.exists(p):
            os.remove(p)
    return code, out, time.time() - t


def mem_split_estimate(b_src):
    """Part b's reservation: the -O0 compile of the long functions alone (measured, LLVM 19: 1.5 GB for the 4.6 MB part
    of c_03d9.c, 1.2 GB for the 12.7 MB part of c_00a3.c); 400 bytes per byte of source, at least MEM_FLOOR."""
    return min(MEM_BUDGET, max(MEM_FLOOR[0], os.path.getsize(b_src) * 400))


def ram_bytes():
    """This machine's RAM in bytes (Linux: /proc/meminfo, macOS: hw.memsize), or 0 when unknown."""
    try:
        if sys.platform == 'darwin':
            return int(subprocess.run(['sysctl', '-n', 'hw.memsize'], capture_output=True, text=True).stdout.strip())
        with open('/proc/meminfo') as f:
            return next(int(l.split()[1]) for l in f if l.startswith('MemTotal:')) << 10
    except (OSError, StopIteration, ValueError):
        return 0


def limit_memory(jobs):
    """Linux and macOS builds: the compilers share RAM - 4 GB (MEM_BUDGET; at least 2 GB). With current clang a lifted file
    can pass 20 GB before its time limit runs out (c_00a3.c: 12 GB after 30 s), which gets processes killed, not slowed
    down; but most files need far less (clang 9 -O1, measured 2026-10-20: 0.13 GB for the median file, 0.9 GB for the
    largest ones, 1.5 GB for c_013a.c), so a fixed share per job ((RAM - 4 GB) / jobs, the rule before) held a 16 GB
    machine to 3 compilers. Linux: a compiler over its reservation fails and is compiled again with twice as much; over the
    whole budget it takes the next step of the fallback ladder, as on a timeout."""
    global MEM_LIMIT, MEM_BUDGET
    if not POSIX_TARGET:
        return
    ram = ram_bytes()
    if not ram:
        return
    MEM_BUDGET = max(2 << 30, ram - (4 << 30))
    MEM_KEY[0] = MEM_BUDGET
    note = ''
    if TARGET_ARM64:
        # The Frame (2026-11-21, first native build): RAM - 4 GB = 11.3 GB for the compilers, but SteamOS + Steam + the XR
        # service hold ~7 GB of its 15.3 GB, so ~3 GB of Steam went to swap during the build. Here the budget leaves
        # RECOMP_MEM_RESERVE GB (default 3.5: Steam's own share) on top of the 4 GB, and never more than what is available
        # when the build starts, less 1 GB (MemAvailable: a game or anything else running then keeps its memory too).
        reserve = int(float(os.environ.get('RECOMP_MEM_RESERVE', '3.5')) * (1 << 30))
        MEM_BUDGET = max(2 << 30, ram - (4 << 30) - reserve)
        MEM_KEY[0] = MEM_BUDGET
        avail = mem_available()
        if avail and avail - (1 << 30) < MEM_BUDGET:
            MEM_BUDGET = max(2 << 30, avail - (1 << 30))
            note = ', %.1f GB available now' % (avail / 2.0 ** 30)
    if os.environ.get('RECOMP_TARGET') in ('linux', 'linux-arm64'):   # both Linux targets; macOS: no limit (no prlimit; RLIMIT_AS is not enforced there)
        MEM_LIMIT = MEM_BUDGET
    print('compilers share %.1f GB of RAM (%s%s); at most %d at once' % (MEM_BUDGET / 2.0 ** 30, 'each held to its reservation'
          if MEM_LIMIT else 'reservations pace them', note, jobs), flush=True)


def mem_available():
    """Linux: MemAvailable of /proc/meminfo in bytes (0 when unknown)."""
    try:
        with open('/proc/meminfo') as f:
            return next(int(l.split()[1]) for l in f if l.startswith('MemAvailable:')) << 10
    except (OSError, StopIteration, ValueError):
        return 0


MEM_FLOOR = [1536 << 20]                 # the smallest reservation; raised when a file reserved at the floor ran out


def mem_estimate(src, sizes):
    """What one compile of src is expected to need, bytes (a reservation, not a promise): MEM_FLOOR (1.5 GB to begin with)
    at least, 0.75 GB per MB of source, 40 KB per statement of its longest function (the giant functions drive clang's
    memory even when they are compiled unoptimised: c_00a3.c, 215,493 statements). At most MEM_BUDGET."""
    longest = max([n for n, _, _ in sizes] or [0])
    return min(MEM_BUDGET, max(MEM_FLOOR[0], os.path.getsize(src) * 768, longest * (40 << 10)))


def mem_floor_raise(want):
    """Linux: a compile reserved at the floor ran out: this clang needs more for an ordinary file than the floor assumes,
    so the next files start a quarter higher (instead of each running out once)."""
    with _mem_cv:
        if want == MEM_FLOOR[0]:             # (not a large file's own estimate; not already raised by another compile)
            MEM_FLOOR[0] = min(MEM_BUDGET, want + want // 4)


_mem_cv = threading.Condition()
_mem_used = 0
_mem_queue = collections.deque()


def mem_acquire(n):
    """Wait until n bytes of MEM_BUDGET are free, first come first served (a large reservation is not overtaken forever).
    Returns what was granted, to be given back with mem_release: n is cut to MEM_BUDGET, because a reservation larger than
    the whole budget waits at the head of the queue for room nothing can free - every compile behind it stops with no
    compiler running and no message (2026-10-22, issue 75: the inlined compile of c_00a3.c asked 16.4 GB of the 12 GB a
    16 GB Mac shares and step 8 sat there for good; its reservation now is the whole budget, so it runs alone)."""
    global _mem_used
    n = min(n, MEM_BUDGET)
    me = object()
    with _mem_cv:
        _mem_queue.append(me)
        try:
            t0 = time.time()
            while _mem_queue[0] is not me or _mem_used + n > MEM_BUDGET:
                _mem_cv.wait(MEM_WAIT_TICK)          # timed, so a wait that does not end can say how long it has been
                if (MEM_WAIT_WARN and _mem_queue[0] is me and _mem_used + n > MEM_BUDGET
                        and time.time() - t0 >= MEM_WAIT_WARN):
                    print('  waiting %.0fs for %.1f GB of RAM (%.1f of %.1f GB held by other compiles)' % (
                        time.time() - t0, n / 2.0 ** 30, _mem_used / 2.0 ** 30, MEM_BUDGET / 2.0 ** 30), flush=True)
                    t0 = time.time()
            _mem_queue.popleft()
            _mem_used += n
        finally:
            if me in _mem_queue:                     # Ctrl+C while waiting: no dead entry left at the head
                _mem_queue.remove(me)
            _mem_cv.notify_all()
    return n


def mem_release(n):
    global _mem_used
    with _mem_cv:
        _mem_used -= n
        _mem_cv.notify_all()


def out_of_memory(code, out):
    return MEM_LIMIT and code and ('out of memory' in out or 'bad_alloc' in out or 'Cannot allocate memory' in out or code < 0)


OOM = 'out of memory'                    # run_limited's output when the memory limit stopped the compiler


def run_limited(cmd, limit, mem=0):
    """Run a compiler with a time limit (and on Linux an address-space limit: mem, else MEM_LIMIT). Returns (exit code,
    output, seconds); the exit code is None when a limit stopped it, the output then OOM for memory and '' for time."""
    t = time.time()
    if MEM_LIMIT:
        cmd = ['prlimit', '--as=%d' % (mem or MEM_LIMIT), '--'] + cmd      # util-linux; inherited by clang's -cc1 child
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, start_new_session=(os.name != 'nt'))
    try:
        out, _ = p.communicate(timeout=limit)
        out = out.decode('latin-1')
        if out_of_memory(p.returncode, out):         # the whole output: clang's stack dump after "LLVM ERROR: out of memory" fills the tail
            return None, OOM, time.time() - t
        return p.returncode, out[-2000:], time.time() - t
    except subprocess.TimeoutExpired:
        if os.name == 'nt':
            subprocess.run(['taskkill', '/PID', str(p.pid), '/T', '/F'], capture_output=True)      # this compiler and its -cc1 child only
        else:
            os.killpg(p.pid, signal.SIGKILL)                     # the clang wrapper's process group
        p.communicate()
        return None, '', time.time() - t


_ladder_lock = threading.Lock()
_ladders = {}


def ladder(objdir):
    """<obj dir>\\_ladder.json: for each file that needed the fallback ladder, the step that succeeded and a hash of what was
    compiled (source, flags, limits). The same file compiled again starts at that step, skipping the time limits it is known
    to exceed (up to 3 x TIME_LIMIT per file); the object is the one the full ladder makes."""
    with _ladder_lock:
        if objdir not in _ladders:
            p = os.path.join(objdir, '_ladder.json')
            try:
                with open(p) as f:
                    _ladders[objdir] = json.load(f)
            except (OSError, ValueError):
                _ladders[objdir] = {}
        return _ladders[objdir]


def ladder_save(objdir, name, key, step):
    with _ladder_lock:
        d = _ladders.setdefault(objdir, {})
        if d.get(name) == [key, step]:
            return
        if step:
            d[name] = [key, step]
        else:
            d.pop(name, None)
        with open(os.path.join(objdir, '_ladder.json'), 'w') as f:
            json.dump(d, f, indent=0, sort_keys=True)


_hints = {}


def hints(objdir):
    """What earlier compiles of the same file took: <obj dir>\\_times.json (this PC) on top of build_hints.json beside this
    script (measured on a full build and shipped). For each object: [sha1 of its source + flags, seconds, inlined]
    (inlined: 1 the inlined compile made the object, 0 it went over RECOMP_INLINE_LIMIT or failed). A first build then
    starts the slowest files first (by time, not size: the tail of the build was a few big files started late) and does
    not spend RECOMP_INLINE_LIMIT seconds on each of the ~400 files known to fall back (20 s x 400 / 12 jobs ~ 11 min).
    A hash that does not match (other lifter output, other flags) is ignored: then the old way (size order, one try)."""
    with _ladder_lock:
        if objdir not in _hints:
            d = {}
            for p in (os.path.join(HERE, 'build_hints.json'), os.path.join(objdir, '_times.json')):
                try:
                    with open(p) as f:
                        d.update(json.load(f))
                except (OSError, ValueError):
                    pass
            _hints[objdir] = d
        return _hints[objdir]


def hint_key(src):
    with open(src, 'rb') as f:
        return hashlib.sha1(f.read() + repr((OPT, EXTRA, INLINE)).encode()).hexdigest()


def hint_save(objdir, name, key, secs, inlined):
    d = hints(objdir)
    with _ladder_lock:
        d[name] = [key, round(secs, 1), inlined]


def hints_write(objdir):
    """Writes _times.json (the files compiled now plus what was known)."""
    d = _hints.get(objdir)
    if d:
        with open(os.path.join(objdir, '_times.json'), 'w') as f:
            json.dump(d, f, indent=0, sort_keys=True)


def long_functions(src, obj, limit):
    """The functions of src over `limit` statements, the way this build treats them. SPLIT: in a unit of their own
    (split_source); returns (part a, <obj>.part_a.o, None, part b, count, statements), or (src, obj, None, None, 0, 0) when
    there are none. Otherwise `optnone` through a header: (src, obj, header, None, count, statements)."""
    a = None
    if SPLIT:
        a, b, count, lines = split_source(src, obj, limit)
    if SPLIT and count >= 0:
        if a:
            hdr = obj[:-4] + '.optnone.h'
            if os.path.exists(hdr):
                os.remove(hdr)
            return a, obj[:-4] + '.part_a.o', None, b, count, lines
        return src, obj, None, None, 0, 0
    hdr, count, lines = optnone_header(src, obj, limit)
    return src, obj, hdr, None, count, lines


def long_note(count, limit, lines):
    if SPLIT:
        return '%d function(s) over %d statements at %s in a unit of their own (%d statements)' % (count, limit, SPLIT_OPT, lines)
    return '%d function(s) over %d statements unoptimised (%d statements)' % (count, limit, lines)


def compile_file(src, obj, base_flags, time_limit=None, extra=None):
    """Compile with the fallback ladder. Returns (exit code, output, seconds, description of the fallback or '')."""
    time_limit = time_limit or TIME_LIMIT
    extra = EXTRA if extra is None else extra
    total = 0.0
    objdir, name = os.path.dirname(obj), os.path.basename(obj)
    with open(src, 'rb') as f:
        key = hashlib.sha1(f.read() + repr((OPT, extra, base_flags, OPTNONE_LINES, time_limit) + ((MEM_KEY[0],) if MEM_LIMIT else ())
                                           + (('split', SPLIT_OPT) if SPLIT else ())).encode()).hexdigest()
    known = ladder(objdir).get(name)
    first = known[1] if known and known[0] == key else 0
    want = mem_estimate(src, function_sizes(src)) if MEM_BUDGET and not SPLIT else 0      # Linux / macOS: this compile's share of RAM
    raised = ''
    tried = None
    why = 'time limit %.0fs%s exceeded three times' % (time_limit, ' or memory limit %.1f GB' % (MEM_LIMIT / 2.0 ** 30) if MEM_LIMIT else '')
    # linux-arm64: a budget cut to what was available at the start (MEM_BUDGET under MEM_KEY) does not record a later step
    # than the one known for the key: a build beside a running game must not leave its fallbacks to every later build
    capped = MEM_BUDGET < MEM_KEY[0]
    for step, limit in enumerate((OPTNONE_LINES, OPTNONE_LINES // 4, OPTNONE_LINES // 16)):
        if step < first:
            continue
        csrc, cobj, hdr, b_src, count, lines = long_functions(src, obj, limit)
        if step and not count:
            continue
        # Linux / macOS: as many functions unoptimised as in the attempt that just failed is the same set (each limit's set
        # holds the previous one), so the same compile, which fails the same way (c_03d9.c: one function of 77,061
        # statements and the rest under 1,250 ran three identical times into the 180 s limit)
        if POSIX_TARGET and count == tried:
            continue
        tried = count
        if SPLIT and MEM_BUDGET:                     # part a's own estimate (its longest function is now under the limit)
            want = max(want, mem_estimate(csrc, function_sizes(csrc)))
        cmd = [CLANG, '-c', OPT] + extra + base_flags + (['-include', hdr] if hdr else []) + [csrc, '-o', cobj]
        while True:
            if want:
                want = mem_acquire(want)                 # what is granted (never over the budget) is what is given back
            try:
                code, out, secs = run_limited(cmd, time_limit, want)
            finally:
                if want:
                    mem_release(want)
            total += secs
            if code is None and out == OOM and want and want < MEM_BUDGET:     # Linux: over its reservation, not the budget
                mem_floor_raise(want)
                want = min(MEM_BUDGET, want * 2)
                raised = ' (memory reservation raised to %.1f GB)' % (want / 2.0 ** 30)
                continue
            break
        if code == 0 and b_src:
            code, out2, secs = split_finish(b_src, obj, extra, base_flags)
            out = (out + out2)[-2000:]
            total += secs
            if code:                     # part b or the join failed: the whole file at -O0 below, as before SPLIT (not saved)
                print('  %s: part b of the split failed (%s); the whole file at -O0' % (name, out.strip()[-300:]), flush=True)
                why, capped = 'part b of the split failed', True
                break
        if code is not None:
            note = ''
            if count:
                note = long_note(count, limit, lines)
            if code == 0 and not (capped and step > first):
                ladder_save(objdir, name, key, step)
            return code, out, total, note + (' (step known from _ladder.json)' if first else '') + raised
    if SPLIT:
        split_clean(obj)
    cmd = [CLANG, '-c', '-O0'] + extra + base_flags + [src, '-o', obj]
    if SPLIT and MEM_BUDGET and not want:         # (every step skipped: known from _ladder.json)
        want = mem_estimate(src, function_sizes(src))
    if want:
        want = mem_acquire(want)
    try:
        r = subprocess.run(cmd, capture_output=True, text=True)
    finally:
        if want:
            mem_release(want)
    if r.returncode == 0 and not (capped and 3 > first):
        ladder_save(objdir, name, key, 3)
    return r.returncode, (r.stdout + r.stderr)[-2000:], total, 'WHOLE FILE at -O0 (%s)' % why


_tries = collections.Counter()
_tries_lock = threading.Lock()
_jobs = [0]


def compile_file_inline(src, obj, base_flags, time_limit=None):
    """With RECOMP_INLINE: ONE inlined compile with a short time limit (RECOMP_INLINE_LIMIT, 20 s); a file that takes longer
    (the biggest generated files: inlining made the whole game compile 2-4x slower, testers 2026-10-21 "40 min -> over an
    hour") or fails is compiled the normal way (compile_file, its own ladder), so the slowdown per file is bounded. The
    files that compile fast, most of them, keep the faster code. Without RECOMP_INLINE: compile_file. linux-arm64 at -O2
    (INLINE_ONE_TRY): compile_file with the inlined helpers, its ladder and time limit, and no separate try."""
    if not INLINE:
        r = compile_file(src, obj, base_flags, time_limit)
        if r[0] == 0:
            hint_save(os.path.dirname(obj), os.path.basename(obj), hint_key(src), r[2], 0)
        return r
    with _tries_lock:
        _tries[obj] += 1
        n = _tries[obj]
    objdir, name = os.path.dirname(obj), os.path.basename(obj)
    key = hint_key(src)
    known = hints(objdir).get(name)
    if INLINE_ONE_TRY and n == 1:        # (a retry round, build.py with fewer jobs: the normal way, at the end)
        r = compile_file(src, obj, base_flags, time_limit, EXTRA + [INLINE_FLAG])
        if r[0] == 0:
            hint_save(objdir, name, key, r[2], 1)
            return r
        code2, out2, secs2, note2 = compile_file(src, obj, base_flags, time_limit)      # failed inlined: as the separate try did
        if code2 == 0:
            hint_save(objdir, name, key, secs2, 0)
        return code2, out2, r[2] + secs2, 'helpers not inlined (the inlined compile failed)' + ('; ' + note2 if note2 else '')
    if n == 1 and known and known[0] == key and known[2] == 0:
        code, out, secs, note = compile_file(src, obj, base_flags, time_limit)
        hint_save(objdir, name, key, secs, 0)
        return code, out, secs, 'helpers not inlined (known to go over %.0f s)' % INLINE_LIMIT + ('; ' + note if note else '')
    if n == 1:
        csrc, cobj, hdr, b_src, count, lines = long_functions(src, obj, OPTNONE_LINES)
        cmd = [CLANG, '-c', OPT] + EXTRA + [INLINE_FLAG] + base_flags + (['-include', hdr] if hdr else []) + [csrc, '-o', cobj]
        # the inlined compile holds more than the plain one (the inliner's IR), but never more than the whole budget: a
        # request over it is one mem_acquire can never grant and stops every compile of the step (2026-10-22, issue 75).
        # A file that needs that much then runs alone instead.
        want = min(MEM_BUDGET, mem_estimate(csrc, function_sizes(csrc)) * 2) if MEM_BUDGET else 0
        if want:
            want = mem_acquire(want)
        try:
            # a file known to make it inlined (same source and flags) gets room for a busier PC: without it ~30 of the files
            # measured at 15-19 s went over the limit on the next full build and lost the inlined helpers
            limit = max(INLINE_LIMIT, 2 * known[1] + 5) if known and known[0] == key and known[2] == 1 else INLINE_LIMIT
            code, out, secs = run_limited(cmd, limit, want)
        finally:
            if want:
                mem_release(want)
        if code == 0 and b_src:          # part b: plain -O0 (split_finish)
            code, out2, secs2 = split_finish(b_src, obj, EXTRA, base_flags)
            out, secs = (out + out2)[-2000:], secs + secs2
            if code:                                 # (part b failed: the normal way below, which says why)
                code = 1
        if code == 0:
            hint_save(objdir, name, key, secs, 1)
            return 0, out, secs, (long_note(count, OPTNONE_LINES, lines) if count else '')
        code2, out2, secs2, note2 = compile_file(src, obj, base_flags, time_limit)
        if code2 == 0:
            hint_save(objdir, name, key, secs2, 0)
        return code2, out2, secs + secs2, ('helpers not inlined (%s)' % ('over %.0f s' % INLINE_LIMIT if code is None else 'the inlined compile failed')) + ('; ' + note2 if note2 else '')
    return compile_file(src, obj, base_flags, time_limit)      # a retry round (build.py, fewer jobs): the normal way



_report = []


def report(objdir):
    if not _report:
        return
    with open(os.path.join(objdir, '_opt_report.txt'), 'a') as f:
        f.write('# %s  %s %s %s  optnone over %d statements, time limit %.0fs\n' % (time.strftime('%Y-%m-%d %H:%M'), CLANG, OPT, ' '.join(EXTRA), OPTNONE_LINES, TIME_LIMIT))
        f.writelines(line + '\n' for line in _report)
    del _report[:]


# ---- main: build.compile_one replaced -------------------------------------------------------------------------
def compile_one_main(name):
    src = os.path.join(build.GEN, name)
    obj = os.path.join(build.OBJ, name[:-2] + '.obj')
    code, out, secs, note = compile_file_inline(src, obj, ['-w', '-fno-strict-aliasing', '-I', HERE, '-I', build.GEN] + build.NOCRT_FLAGS + build.PLAT_FLAGS)
    if note or secs > 60:
        line = '%-12s %6.0fs  %s' % (name, secs, note)
        _report.append(line)
        print('  ' + line, flush=True)
    return name, code, out, secs


def split_upgrade(objdir):
    """SPLIT, once per object folder (stamp _split.txt): the objects an older build made WHOLE FILE at -O0 (_ladder.json step
    3) are compiled again, now with only their long functions at -O0 (first Frame build: c_00a3.c, c_013a.c, a few
    minutes). The objects made with `optnone` functions are kept: the same functions at the same level, the rest of the
    file was at -O2 already; they take the split way when their source changes."""
    if not SPLIT:
        return
    stamp = os.path.join(objdir, '_split.txt')
    if os.path.exists(stamp):
        return
    redo = sorted(n for n, (_, step) in ladder(objdir).items() if step == 3)
    for n in redo:
        p = os.path.join(objdir, n)
        if os.path.exists(p):
            os.remove(p)
    if redo:
        print('compiled again once, now with only their longest functions at %s instead of the whole file: %s' % (SPLIT_OPT, ' '.join(redo)), flush=True)
    with open(stamp, 'w') as f:
        f.write('%s\n' % SPLIT_OPT)


def cmd_main(jobs):
    global SPLIT
    SPLIT = SPLIT_WANTED
    build.compile_one = compile_one_main
    _jobs[0] = jobs
    limit_memory(jobs)
    t0 = time.time()
    # objects compiled with the other RECOMP_INLINE setting are compiled again (the compile skips an object newer than its
    # source, whatever flags made it): a build from before inlining existed gets the faster code on its next update
    stamp, want = os.path.join(build.OBJ, '_inline.txt'), '1' if INLINE else '0'
    try:
        have = open(stamp).read().strip()
    except OSError:
        have = None                          # no stamp: objects from before the stamp existed were compiled without inlining
    old = [n for n in os.listdir(build.OBJ) if n.endswith('.obj')] if os.path.isdir(build.OBJ) else []
    if old and have != want and (have is not None or want == '1'):
        print('the game objects were compiled %s the inlined helpers: all %d are compiled again (once)' % ('with' if have == '1' else 'without', len(old)), flush=True)
        for n in old:
            os.remove(os.path.join(build.OBJ, n))
    os.makedirs(build.OBJ, exist_ok=True)
    with open(stamp, 'w') as f:          # now, not after the compile: an interrupted or failed compile must not lose its objects on the next start
        f.write(want + chr(10))
    split_upgrade(build.OBJ)
    def order_key(n):                    # slowest first by the time it took before (same source and flags), else by size
        h = hints(build.OBJ).get(n[:-2] + '.obj')
        size = os.path.getsize(os.path.join(build.GEN, n))
        if h and h[0] == hint_key(os.path.join(build.GEN, n)):
            return -h[1]
        return -size * 3e-6                # ~1 MB of source ~ 3 s: unmeasured files sit among the measured ones
    build.order_key = order_key
    try:
        rc = build.cmd_compile(jobs, False)
    finally:
        hints_write(build.OBJ)
    report(build.OBJ)
    if rc:
        return rc
    rc = build.build_game_dll()
    print('main: compile + link %.0fs' % (time.time() - t0))
    return rc


# ---- modules: build_nro.run_clang replaced --------------------------------------------------------------------
def run_clang_nro(job):
    args, label = job
    src, obj = args[-3], args[-1]
    base = [a for a in args[:-3] if a not in ('-c', '-O0', '-O1', '-O2')]
    code, out, secs, note = compile_file(src, obj, base)
    if note or secs > 60:
        line = '%-40s %6.0fs  %s' % (os.path.basename(os.path.dirname(src)) + '/' + label, secs, note)
        _report.append(line)
        print('  ' + line, flush=True)
    return label, code, out, secs


def cmd_nro(jobs, names):
    build_nro.run_clang = run_clang_nro
    limit_memory(jobs)
    if not names:
        names = [e['name'] for e in build_nro.load_map()]
    rc = build_nro.main(names + ['--jobs=%d' % jobs])
    # a compiler that crashed with many running at once usually ran out of memory (clang "due to signal"): the modules are
    # built again with far fewer at a time, then one; only the objects that failed are compiled again
    for retry in (max(1, jobs // 4), 1):
        if not rc or retry >= jobs:
            continue
        print('building the modules again with %d job(s): some compiles failed (usually out of memory with %d at once)' % (retry, jobs), flush=True)
        rc = build_nro.main(names + ['--jobs=%d' % retry])
    report(os.path.join(HERE, 'obj_nro' + os.environ['RECOMP_NRO_SUFFIX']))
    return rc


if __name__ == '__main__':
    cmd = sys.argv[1]
    if cmd == 'main':
        sys.exit(cmd_main(int(sys.argv[2]) if len(sys.argv) > 2 else 28))
    if cmd == 'nro':
        rest = sys.argv[2:]
        jobs = int(rest.pop(0)) if rest and rest[0].isdigit() else 28
        sys.exit(cmd_nro(jobs, rest))
    if cmd == 'one':
        SPLIT = SPLIT_WANTED
        os.makedirs(build.OBJ, exist_ok=True)
        src = sys.argv[2] if os.path.isabs(sys.argv[2]) else os.path.join(build.GEN, sys.argv[2])
        obj = os.path.join(os.environ.get('RECOMP_ONE_OUT', build.OBJ), os.path.basename(src)[:-2] + '.obj')
        code, out, secs, note = compile_file(src, obj, ['-w', '-fno-strict-aliasing', '-I', HERE, '-I', os.path.dirname(src)], float(sys.argv[3]) if len(sys.argv) > 3 else None)
        print('%s: exit %s  %.1fs  %s  %s' % (os.path.basename(src), code, secs, note or 'fully optimised', out.strip()[:500]))
    if cmd == 'sizes':
        rows = []
        for n in sorted(os.listdir(sys.argv[2])):
            if n.startswith('c_') and n.endswith('.c'):
                rows += [(s, name, n) for s, name, _ in function_sizes(os.path.join(sys.argv[2], n))]
        rows.sort(reverse=True)
        print('functions: %d   statements: %d' % (len(rows), sum(r[0] for r in rows)))
        for limit in (1000, 2000, 4000, 8000, 16000, 32000):
            big = [r for r in rows if r[0] > limit]
            print('  over %6d statements: %5d functions, %9d statements (%.1f%%)' % (limit, len(big), sum(r[0] for r in big), 100.0 * sum(r[0] for r in big) / sum(r[0] for r in rows)))
        for r in rows[:25]:
            print('  %7d  %s  %s' % r)
