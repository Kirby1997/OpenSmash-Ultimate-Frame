/* plat_crash.c - async-signal-safe crash reports for aarch64 POSIX hosts. Interface and behaviour: plat_crash.h.
 * Linux aarch64 for real; builds on macOS arm64 too, for tests/port/crash. Nothing here exists on Windows. */
#ifndef _WIN32
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "plat_crash.h"
#include <signal.h>
#include <setjmp.h>
#include <stdio.h>
#ifndef __APPLE__
#include <ucontext.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#else
#include <link.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif
#if !defined(__aarch64__)
#error "plat_crash.c: aarch64 only (x86-64 hosts keep hostrt.c's handler)"
#endif

/* ------------------------------------------------------------------------------------------------ state */
static char g_path[4096];                         /* report file, resolved before any crash */
static crash_section_fn g_section;
static crash_callback_fn g_callback;
static int (*g_filter)(void *);
static int g_exit_code = 4, g_exit_mode, g_timeout = 20, g_installed;
static int g_spare = -1;                          /* descriptor kept free so that the report file can be opened at EMFILE */
static int g_pipe[2] = {-1, -1};                  /* crash_read */
static size_t g_page = 4096;
static struct timespec g_start;
static pthread_key_t g_key;
static pthread_once_t g_key_once = PTHREAD_ONCE_INIT;
static volatile int g_owner;                      /* tid of the thread that is reporting, 0 = none */
static int g_sig;                                 /* its signal, for the alarm */
static sigjmp_buf g_jmp;
static volatile int g_jmp_on;
static int g_fd = -1;                             /* report file during a report */
static int g_console = 2;
static char g_buf[16384];
static size_t g_len;

static int my_tid(void) {
#ifdef __APPLE__
    uint64_t t = 0; pthread_threadid_np(NULL, &t); return (int)t;
#else
    return (int)syscall(SYS_gettid);
#endif
}

/* ------------------------------------------------------------------------------------------------ output */
static void write_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t k = write(fd, p, n);
        if (k < 0) { if (errno == EINTR) continue; return; }
        p += k; n -= (size_t)k;
    }
}
static void flush_buf(void) {
    if (g_len) {
        if (g_fd >= 0) write_all(g_fd, g_buf, g_len);
        if (g_console >= 0) write_all(g_console, g_buf, g_len);
    }
    g_len = 0;
}
static void emit(const char *s, size_t n) {
    while (n) {
        size_t c = sizeof g_buf - g_len;
        if (!c) { flush_buf(); continue; }
        if (c > n) c = n;
        memcpy(g_buf + g_len, s, c);
        g_len += c; s += c; n -= c;
    }
}
void crash_put(const char *s) { emit(s, strlen(s)); }
void crash_put_hex(uint64_t v, int width) {
    char t[16];
    int n = 0;
    do { t[n++] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v && n < 16);
    while (n < width && n < 16) t[n++] = '0';
    char o[16];
    for (int i = 0; i < n; i++) o[i] = t[n - 1 - i];
    emit(o, (size_t)n);
}
void crash_put_dec(long long v) {
    char t[24];
    int n = 0;
    unsigned long long u = v < 0 ? 0ULL - (unsigned long long)v : (unsigned long long)v;
    do { t[n++] = (char)('0' + u % 10); u /= 10; } while (u);
    if (v < 0) t[n++] = '-';
    char o[24];
    for (int i = 0; i < n; i++) o[i] = t[n - 1 - i];
    emit(o, (size_t)n);
}
static void put_h(uint64_t v) { crash_put("0x"); crash_put_hex(v, 0); }
static void put_h16(uint64_t v) { crash_put_hex(v, 16); }
static void put_name(const char *s) { emit(s, strnlen(s, 64)); }

/* ------------------------------------------------------------------------------------------------ memory reads */
uint64_t crash_strip_pac(uint64_t a) { return a & 0x0000ffffffffffffULL; }
/* write(2) from memory that is not readable fails with EFAULT instead of faulting: copy through a pipe. One reader at a time (the
 * reporting thread). */
int crash_read(uint64_t addr, void *out, size_t n) {
    if (!n) return 1;
    if (n > 256 || g_pipe[1] < 0 || addr + n < addr) return 0;
    ssize_t k = write(g_pipe[1], (const void *)(uintptr_t)addr, n);
    if (k <= 0) return 0;
    char tmp[256];
    ssize_t r = read(g_pipe[0], tmp, (size_t)k);
    if (r != (ssize_t)n || k != (ssize_t)n) return 0;
    memcpy(out, tmp, n);
    return 1;
}
int crash_esr_access(const void *reserved, size_t n) {
    const uint8_t *p = reserved, *e = p + n;
    while (p + 8 <= e) {
        uint32_t magic, size; memcpy(&magic, p, 4); memcpy(&size, p + 4, 4);
        if (!magic || size < 8 || size > (size_t)(e - p)) break;
        if (magic == 0x45535201u && size >= 16) {         /* ESR_MAGIC: struct esr_context { head; u64 esr; } */
            uint64_t esr; memcpy(&esr, p + 8, 8);
            uint32_t ec = (uint32_t)(esr >> 26) & 0x3f;
            return ec == 0x24 || ec == 0x25 ? ((esr >> 6) & 1 ? 'w' : 'r') : 0;     /* data aborts only; WnR is bit 6 */
        }
        p += size;
    }
    return 0;
}

/* ------------------------------------------------------------------------------------------------ modules */
#define MAXMOD 512
static crash_mod g_modtab[2][MAXMOD];
static int g_modn[2], g_modcur;
static pthread_mutex_t g_modlock = PTHREAD_MUTEX_INITIALIZER;
static const char *base_of(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }
static void mod_add(crash_mod *tab, int *n, uint64_t lo, uint64_t hi, uint64_t base, const char *name) {
    if (*n >= MAXMOD) return;
    crash_mod *m = &tab[(*n)++];
    m->lo = lo; m->hi = hi; m->base = base;
    snprintf(m->name, sizeof m->name, "%s", base_of(name));
}
#ifdef __APPLE__
static void mod_fill(crash_mod *tab, int *n) {
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const struct mach_header_64 *mh = (const struct mach_header_64 *)_dyld_get_image_header(i);
        unsigned long size = 0;
        uint8_t *t = mh ? getsegmentdata(mh, "__TEXT", &size) : NULL;
        if (t) mod_add(tab, n, (uint64_t)(uintptr_t)t, (uint64_t)(uintptr_t)t + size, (uint64_t)(uintptr_t)mh, _dyld_get_image_name(i));
    }
}
#else
static int mod_cb(struct dl_phdr_info *in, size_t sz, void *arg) {
    (void)sz;
    void **a = arg;
    const char *name = in->dlpi_name && in->dlpi_name[0] ? in->dlpi_name : (const char *)a[2];
    for (int k = 0; k < in->dlpi_phnum; k++) {
        const ElfW(Phdr) *ph = &in->dlpi_phdr[k];
        if (ph->p_type == PT_LOAD && (ph->p_flags & PF_X))
            mod_add(a[0], a[1], in->dlpi_addr + ph->p_vaddr, in->dlpi_addr + ph->p_vaddr + ph->p_memsz, in->dlpi_addr, name);
    }
    return 0;
}
static void mod_fill(crash_mod *tab, int *n) {
    char exe[512] = "exe";
    ssize_t k = readlink("/proc/self/exe", exe, sizeof exe - 1);
    exe[k > 0 ? k : 0] = 0;
    if (k <= 0) snprintf(exe, sizeof exe, "exe");
    void *a[3] = {tab, n, exe};
    dl_iterate_phdr(mod_cb, a);
}
#endif
void crash_modules_refresh(void) {
    pthread_mutex_lock(&g_modlock);
    int next = !__atomic_load_n(&g_modcur, __ATOMIC_ACQUIRE), n = 0;
    mod_fill(g_modtab[next], &n);
    g_modn[next] = n;
    __atomic_store_n(&g_modcur, next, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_modlock);
}
int crash_mod_find(const crash_mod *tab, int n, uint64_t pc, const crash_mod **out) {
    for (int i = 0; i < n; i++) if (pc >= tab[i].lo && pc < tab[i].hi) { *out = &tab[i]; return 1; }
    return 0;
}

/* /proc/self/maps by open/read: for an address the snapshot does not know, and the mapping around a fault address. */
typedef struct { uint64_t lo, hi, off; char perms[5], name[40]; int ok; } MapLine;
static int maps_parse(const char *s, MapLine *m) {
    char *e;
    memset(m, 0, sizeof *m);
    m->lo = strtoull(s, &e, 16);
    if (*e != '-') return 0;
    m->hi = strtoull(e + 1, &e, 16);
    if (*e != ' ') return 0;
    memcpy(m->perms, e + 1, 4);
    m->off = strtoull(e + 6, &e, 16);
    for (int f = 0; f < 2 && *e; f++) { while (*e == ' ') e++; while (*e && *e != ' ') e++; }      /* dev, inode */
    while (*e == ' ') e++;
    snprintf(m->name, sizeof m->name, "%s", *e == '/' ? base_of(e) : e);
    m->ok = 1;
    return 1;
}
int crash_maps_line(const char *line, uint64_t pc, char *name, size_t nsz, uint64_t *off) {
    MapLine m;
    if (!maps_parse(line, &m) || pc < m.lo || pc >= m.hi || !m.name[0]) return 0;
    snprintf(name, nsz, "%s", m.name);
    *off = pc - m.lo + m.off;
    return 1;
}
static const char *g_maps_file = "/proc/self/maps";      /* tests point it at a file of their own */
void crash_set_maps_file(const char *path) { g_maps_file = path; }
/* in = the mapping that holds addr, prev = the one before it, next = the first above it */
static void maps_find(uint64_t addr, MapLine *in, MapLine *prev, MapLine *next) {
    in->ok = prev->ok = next->ok = 0;
    int fd = open(g_maps_file, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    char chunk[2048], line[512];
    size_t ll = 0;
    MapLine cur;
    for (;;) {
        ssize_t k = read(fd, chunk, sizeof chunk);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) break;
        for (ssize_t i = 0; i < k; i++) {
            if (chunk[i] != '\n') { if (ll < sizeof line - 1) line[ll++] = chunk[i]; continue; }
            line[ll] = 0; ll = 0;
            if (!maps_parse(line, &cur)) continue;
            if (addr >= cur.lo && addr < cur.hi) *in = cur;
            else if (cur.hi <= addr) *prev = cur;
            else if (!next->ok && cur.lo > addr) { *next = cur; close(fd); return; }
        }
    }
    close(fd);
}
int crash_module_of(uint64_t pc, char *name, size_t nsz, uint64_t *off) {
    int cur = __atomic_load_n(&g_modcur, __ATOMIC_ACQUIRE);
    const crash_mod *m;
    if (crash_mod_find(g_modtab[cur], g_modn[cur], pc, &m)) { snprintf(name, nsz, "%s", m->name); *off = pc - m->base; return 1; }
    MapLine in, a, b;
    maps_find(pc, &in, &a, &b);
    if (in.ok && in.name[0] && in.name[0] != '[') { snprintf(name, nsz, "%s", in.name); *off = pc - in.lo + in.off; return 1; }
    return 0;
}
void crash_put_module(uint64_t addr) {
    char n[48]; uint64_t off;
    if (crash_module_of(addr, n, sizeof n, &off)) { put_name(n); crash_put("+"); put_h(off); }
    else crash_put("?");
}

/* ------------------------------------------------------------------------------------------------ report text */
const char *crash_signal_text(int sig, int code) {
    switch (sig) {
    case SIGSEGV: return code == SEGV_MAPERR ? "SIGSEGV, SEGV_MAPERR: address not mapped" : code == SEGV_ACCERR ? "SIGSEGV, SEGV_ACCERR: no permission for the access" : "SIGSEGV";
    case SIGBUS:  return code == BUS_ADRALN ? "SIGBUS, BUS_ADRALN: misaligned address" : code == BUS_ADRERR ? "SIGBUS, BUS_ADRERR: no such physical address" : "SIGBUS";
    case SIGILL:  return code == ILL_ILLOPC ? "SIGILL, ILL_ILLOPC: illegal opcode" : "SIGILL";
    case SIGFPE:  return code == FPE_INTDIV ? "SIGFPE, FPE_INTDIV: integer divide by zero" : "SIGFPE";
    case SIGABRT: return "SIGABRT, abort()";
    default: return "?";
    }
}
static void put_dec2(long long v) { if (v < 10) crash_put("0"); crash_put_dec(v); }
static void put_stamp(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long long t = ts.tv_sec, d = t / 86400, s = t % 86400;
    d += 719468;                                       /* days since 0000-03-01 (Howard Hinnant's civil_from_days) */
    long long era = d / 146097, doe = d % 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = yoe + era * 400, doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    long long dd = doy - (153 * mp + 2) / 5 + 1, mm = mp < 10 ? mp + 3 : mp - 9;
    if (mm <= 2) y++;
    crash_put_dec(y); crash_put("-"); put_dec2(mm); crash_put("-"); put_dec2(dd); crash_put(" ");
    put_dec2(s / 3600); crash_put(":"); put_dec2(s / 60 % 60); crash_put(":"); put_dec2(s % 60); crash_put(" UTC");
}

static void part_header(const crash_info *ci) {
    crash_put("=== "); put_stamp(); crash_put("  HOST SIGNAL "); crash_put_dec(ci->sig); crash_put(" (");
    crash_put(crash_signal_text(ci->sig, ci->code)); crash_put(") at pc "); put_h(ci->pc); crash_put(" (");
    crash_put_module(ci->pc); crash_put(")\n");
    if (ci->sig == SIGSEGV || ci->sig == SIGBUS) {
        crash_put("  "); crash_put(ci->access == 'w' ? "writing" : ci->access == 'r' ? "reading" : "accessing");
        crash_put(" address "); put_h(ci->addr); crash_put("\n");
        uint64_t d = ci->addr > ci->sp ? ci->addr - ci->sp : ci->sp - ci->addr;
        if (d < 0x10000) crash_put("  the address is within 64 KiB of the stack pointer: most likely a stack overflow\n");
    }
    uint32_t insn;
    if (crash_read(ci->pc, &insn, 4)) { crash_put("  instruction at pc: "); crash_put_hex(insn, 8); crash_put("\n"); }
    else crash_put("  instruction at pc: not readable\n");
}
static void put_map(const char *tag, const MapLine *m) {
    crash_put("  "); crash_put(tag); put_h16(m->lo); crash_put("-"); put_h16(m->hi); crash_put(" "); emit(m->perms, 4); crash_put(" ");
    put_name(m->name[0] ? m->name : "(anonymous)"); crash_put("\n");
}
static void part_mapping(const crash_info *ci) {
    if (ci->sig != SIGSEGV && ci->sig != SIGBUS) return;
    MapLine in, prev, next;
    maps_find(ci->addr, &in, &prev, &next);
    if (!in.ok && !prev.ok && !next.ok) return;
    if (in.ok) put_map("mapping holding the address: ", &in);
    else { crash_put("  the address is not mapped\n"); if (prev.ok) put_map("mapping before it: ", &prev); if (next.ok) put_map("mapping after it:  ", &next); }
}
static void part_thread(const crash_info *ci) {
    (void)ci;
    char nm[32] = "";
#ifdef __APPLE__
    pthread_getname_np(pthread_self(), nm, sizeof nm);
#else
    prctl(PR_GET_NAME, nm, 0, 0, 0);
#endif
    nm[sizeof nm - 1] = 0;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    crash_put("thread "); crash_put_dec(my_tid()); crash_put(" ("); put_name(nm[0] ? nm : "?"); crash_put(")");
#ifndef __APPLE__
    crash_put(", on logical processor "); crash_put_dec(sched_getcpu());
#endif
    crash_put("; process "); crash_put_dec(getpid()); crash_put(", up "); crash_put_dec((long long)(now.tv_sec - g_start.tv_sec)); crash_put(" s\n");
}
static void part_regs(const crash_info *ci) {
    crash_put("host registers:\n  pc "); put_h16(ci->pc); crash_put("  lr "); put_h16(ci->lr); crash_put("  sp "); put_h16(ci->sp);
    crash_put("  fp "); put_h16(ci->fp); crash_put("\n  pstate "); put_h16(ci->pstate); crash_put("\n");
    for (int i = 0; i < 31; i++) {
        crash_put(i < 10 ? "  x" : " x"); crash_put_dec(i); crash_put(" "); put_h16(ci->x[i]);
        if (i % 4 == 3 || i == 30) crash_put("\n");
    }
}
static void part_stack(const crash_info *ci) {
    uint64_t a = ci->sp - 0x40;
    crash_put("host stack from sp-0x40:\n");
    for (int i = 0; i < 48; i++) {
        uint64_t w;
        if (!crash_read(a + 8u * (unsigned)i, &w, 8)) { crash_put("  (not readable beyond sp"); crash_put(i < 8 ? "-0x" : "+0x"); crash_put_hex(i < 8 ? 0x40 - 8u * (unsigned)i : 8u * (unsigned)i - 0x40, 0); crash_put(")\n"); return; }
        if (i % 2 == 0) { crash_put("  sp"); crash_put(i < 8 ? "-0x" : "+0x"); crash_put_hex(i < 8 ? 0x40 - 8u * (unsigned)i : 8u * (unsigned)i - 0x40, 2); crash_put(" "); }
        put_h16(w); crash_put(i % 2 ? "\n" : "  ");
    }
}
static void part_frames(const crash_info *ci) {
    crash_put("host call stack (frame pointers, innermost first):\n");
    for (int i = 0; i < ci->npcs; i++) {
        crash_put("  #"); crash_put_dec(i); crash_put(" "); put_h16(ci->pcs[i]); crash_put(" "); crash_put_module(ci->pcs[i]); crash_put("\n");
    }
    crash_put(ci->npcs >= CRASH_MAX_FRAMES ? "  (more frames not shown)\n" : "");
}
/* pcs[0] = pc; then lr when the fault came before the function pushed its frame record; then the chain from fp */
static void walk(crash_info *ci) {
    ci->npcs = 0;
    ci->pcs[ci->npcs++] = ci->pc;
    uint64_t fp = ci->fp, lr = crash_strip_pac(ci->lr), first = 0;
    int have_first = 0;
    for (int guard = 0; fp && !(fp & 15) && ci->npcs < CRASH_MAX_FRAMES && guard < 4 * CRASH_MAX_FRAMES; guard++) {
        uint64_t rec[2];
        if (!crash_read(fp, rec, 16)) break;
        uint64_t ret = crash_strip_pac(rec[1]);
        if (!ret) break;
        if (!have_first) { have_first = 1; first = ret; if (lr && lr != ret && lr != ci->pc) ci->pcs[ci->npcs++] = lr; }
        ci->pcs[ci->npcs++] = ret;
        if (rec[0] <= fp) break;
        fp = rec[0];
    }
    if (!have_first && lr && lr != ci->pc && ci->npcs < CRASH_MAX_FRAMES) ci->pcs[ci->npcs++] = lr;
    (void)first;
}

/* ------------------------------------------------------------------------------------------------ the handler */
#define PART(label, call) do { if (!sigsetjmp(g_jmp, 1)) { g_jmp_on = 1; call; g_jmp_on = 0; } \
    else { g_jmp_on = 0; crash_put("  [a fault inside the " label " part of this report; the report goes on]\n"); } flush_buf(); } while (0)

static void gather(crash_info *ci, int sig, siginfo_t *si, void *uc) {
    ucontext_t *u = uc;
    memset(ci, 0, sizeof *ci);
    ci->sig = sig; ci->code = si->si_code; ci->addr = (uint64_t)(uintptr_t)si->si_addr;
#ifdef __APPLE__
    ci->pc = u->uc_mcontext->__ss.__pc; ci->sp = u->uc_mcontext->__ss.__sp; ci->fp = u->uc_mcontext->__ss.__fp;
    ci->lr = u->uc_mcontext->__ss.__lr; ci->pstate = u->uc_mcontext->__ss.__cpsr;
    for (int i = 0; i < 29; i++) ci->x[i] = u->uc_mcontext->__ss.__x[i];
    ci->x[29] = ci->fp; ci->x[30] = ci->lr;
    ci->access = (u->uc_mcontext->__es.__esr >> 26) == 0x24 ? ((u->uc_mcontext->__es.__esr >> 6) & 1 ? 'w' : 'r') : 0;
#else
    ci->pc = u->uc_mcontext.pc; ci->sp = u->uc_mcontext.sp; ci->pstate = u->uc_mcontext.pstate;
    for (int i = 0; i < 31; i++) ci->x[i] = u->uc_mcontext.regs[i];
    ci->fp = ci->x[29]; ci->lr = ci->x[30];
    ci->access = crash_esr_access(u->uc_mcontext.__reserved, sizeof u->uc_mcontext.__reserved);
#endif
}
static void die_with_default(int sig, int sent) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
    sigset_t s;
    sigemptyset(&s); sigaddset(&s, sig);
    sigprocmask(SIG_UNBLOCK, &s, NULL);
    if (sent) kill(getpid(), sig);          /* a sent signal does not come back by itself */
    /* a fault: the handler returns and the instruction faults again, now with the default action */
}
static void finish(int sig, int code) {
    if (g_exit_mode) _exit(g_exit_code);
    die_with_default(sig, code <= 0 || sig == SIGABRT);
}
static void on_alarm(int s) {
    (void)s;
    const char m[] = "crash report: the summaries did not finish in time; ending the process\n";
    write_all(g_console, m, sizeof m - 1);
    if (g_fd >= 0) write_all(g_fd, m, sizeof m - 1);
    if (g_exit_mode) _exit(g_exit_code);
    die_with_default(g_sig, 1);
    _exit(g_exit_code);                     /* still here: nothing delivered */
}
static void handler(int sig, siginfo_t *si, void *uc) {
    int saved = errno, tid = my_tid(), zero = 0;
    if ((sig == SIGSEGV || sig == SIGBUS) && g_filter && g_filter(si->si_addr)) { errno = saved; return; }
    if (!__atomic_compare_exchange_n(&g_owner, &zero, tid, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        if (zero == tid) {                                  /* a fault inside the handler */
            if (g_jmp_on) siglongjmp(g_jmp, 1);
            static const char m[] = "crash report: a fault inside the crash handler itself; ending\n";
            write_all(2, m, sizeof m - 1);
            finish(sig, si->si_code);
            return;
        }
        for (;;) { struct timespec ts = {1, 0}; nanosleep(&ts, NULL); }       /* another thread is reporting: it ends the process */
    }
    g_sig = sig;
    signal(SIGPIPE, SIG_IGN);                               /* a closed console must not end the report */
    if (g_timeout > 0) alarm((unsigned)g_timeout);
    if (g_spare >= 0) { close(g_spare); g_spare = -1; }
    g_fd = g_path[0] ? open(g_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644) : -1;
    int file_err = g_path[0] && g_fd < 0 ? errno : 0;
    static crash_info ci;                                   /* static: the alternate stack is small */
    PART("registers", gather(&ci, sig, si, uc));
    PART("header", part_header(&ci));
    PART("fault address", part_mapping(&ci));
    PART("thread", part_thread(&ci));
    PART("registers", part_regs(&ci));
    PART("stack", part_stack(&ci));
    PART("call stack", (walk(&ci), part_frames(&ci)));
    if (g_section) PART("program", g_section(&ci));
    if (file_err) { crash_put("(the report file could not be opened: errno "); crash_put_dec(file_err); crash_put(")\n"); }
    crash_put("=== end of crash report\n");
    flush_buf();
    if (g_fd >= 0) { close(g_fd); g_fd = -1; }
    if (g_callback) {
        if (!sigsetjmp(g_jmp, 1)) { g_jmp_on = 1; g_callback(&ci); g_jmp_on = 0; }
        else { g_jmp_on = 0; static const char m[] = "crash report: a fault inside the summaries; ending\n"; write_all(2, m, sizeof m - 1); }
    }
    alarm(0);
    errno = saved;
    finish(sig, si->si_code);
}

/* ------------------------------------------------------------------------------------------------ threads, install */
static void thread_exit(void *stack) {
    stack_t ss;
    memset(&ss, 0, sizeof ss);
    ss.ss_flags = SS_DISABLE;
    sigaltstack(&ss, NULL);
    munmap(stack, (size_t)(256 << 10) + g_page);
}
static void make_key(void) { pthread_key_create(&g_key, thread_exit); }
int crash_thread_init(void) {
    static __thread int done;
    if (done) return 0;
    stack_t cur;
    if (!sigaltstack(NULL, &cur) && !(cur.ss_flags & SS_DISABLE)) { done = 1; return 0; }      /* the thread has one already */
    size_t sz = 256 << 10;
    char *m = mmap(NULL, sz + g_page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (m == MAP_FAILED) return -errno;
    mprotect(m, g_page, PROT_NONE);                        /* guard below the stack */
    stack_t ss = {.ss_sp = m + g_page, .ss_size = sz, .ss_flags = 0};
    if (sigaltstack(&ss, NULL)) { int e = errno; munmap(m, sz + g_page); return -e; }
    pthread_once(&g_key_once, make_key);
    pthread_setspecific(g_key, m);                         /* its destructor frees the stack at thread exit */
    done = 1;
    return 0;
}
void crash_set_fault_filter(int (*filter)(void *)) { g_filter = filter; }
int crash_install(const char *report_path, crash_section_fn section, crash_callback_fn callback, int exit_code) {
    if (__atomic_exchange_n(&g_installed, 1, __ATOMIC_ACQ_REL)) { g_section = section; g_callback = callback; g_exit_code = exit_code; return 0; }
    long ps = sysconf(_SC_PAGESIZE);
    g_page = ps > 0 ? (size_t)ps : 4096;
    g_section = section; g_callback = callback; g_exit_code = exit_code;
    snprintf(g_path, sizeof g_path, "%s", report_path ? report_path : "");
    clock_gettime(CLOCK_MONOTONIC, &g_start);
    const char *v = getenv("SSBU_CRASH_TIMEOUT");
    if (v && *v) g_timeout = atoi(v);
    v = getenv("SSBU_CRASH_EXIT");
    g_exit_mode = v && *v && *v != '0';
    if (!pipe(g_pipe)) for (int i = 0; i < 2; i++) { fcntl(g_pipe[i], F_SETFD, FD_CLOEXEC); fcntl(g_pipe[i], F_SETFL, O_NONBLOCK); }
    g_spare = open("/dev/null", O_RDONLY | O_CLOEXEC);
    crash_modules_refresh();
    crash_thread_init();                                  /* a failure shows as an overflow without report, nothing else */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    static const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
    for (unsigned i = 0; i < sizeof sigs / sizeof *sigs; i++) if (sigaction(sigs[i], &sa, NULL)) return -errno;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm;
    sa.sa_flags = SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);
    return 0;
}
#endif /* !_WIN32 */
