/* plat_crash.h - async-signal-safe crash reports for aarch64 POSIX hosts (C4).
 *
 * hostrt.c's own handler (crash_signal) prints with stdio from inside the signal handler: a crash with the heap or a stdio lock
 * damaged or held loses the report, a stack overflow cannot run it at all, and the report file is opened with fopen. This file
 * is the report path that keeps working then. Used on Linux aarch64 (SSBU_ASYNC_CRASH, below); macOS and x86-64 Linux keep
 * hostrt.c's handler. The file itself also builds on macOS arm64 so that tests/port/crash runs on a development Mac.
 *
 * crash_install() puts one handler on SIGSEGV, SIGBUS, SIGILL, SIGFPE and SIGABRT (SA_SIGINFO | SA_ONSTACK | SA_NODEFER).
 * A crash is reported in two steps:
 *  1. Async-signal-safe: no malloc, no stdio, no locks. The report is formatted into a static buffer with the helpers below
 *     and written with write(2) to the report file (path given at crash_install, i.e. resolved at start-up; opened by the handler
 *     with O_APPEND, a spare descriptor covers EMFILE; a run without a crash leaves no file) and to stderr. Memory the report
 *     reads (instruction, stack words, frame records) goes through crash_read, which turns a bad address into a failed write(2)
 *     instead of a second fault. A fault inside a part of the report marks that part and the report goes on.
 *         === <UTC time>  HOST SIGNAL 11 (SIGSEGV, SEGV_MAPERR) at pc 0x... (module+0xoff)
 *           reading|writing address 0x...    (ESR of a data abort; "accessing" when the kernel gave none)
 *           [stack overflow hint]  [mapping that holds / follows the fault address]
 *           instruction at pc
 *         thread, cpu, pid, uptime
 *         host registers (pc lr sp fp pstate x0..x30), host stack words from sp-0x40
 *         host call stack (frame pointers, PAC stripped), each as module+0xoffset
 *         the section of the user (crash_install's `section`: hostrt.c adds the guest side)
 *         === end of crash report
 *     Module names come from a snapshot taken outside the handler (dl_iterate_phdr; refreshed by crash_modules_refresh) and, for
 *     an address it does not know (a library loaded later), from /proc/self/maps read with open/read.
 *  2. The callback (hostrt.c: the run's summaries, which use stdio), bounded by SSBU_CRASH_TIMEOUT seconds (default 20; read at
 *     crash_install): a fault in it ends it, an alarm ends the process. The report of step 1 is already out by then.
 * Then the signal's default action runs (the handler is reset and the fault re-executes, or the signal is sent again), so the exit
 * status names the signal and a core dump is written where core dumps are on (SteamOS: coredumpctl). SSBU_CRASH_EXIT=1 (read at
 * crash_install) instead ends with _exit(exit_code), as the Windows build does (hostrt.c: 4). One report per process: a second
 * thread that crashes meanwhile waits for the first to end the process; a fault in the handler outside a part ends it at once.
 *
 * Stack overflow: the handler runs on a per-thread alternate stack, which every thread that may crash needs:
 * crash_thread_init() (idempotent; freed at thread exit; a stack the thread already has is left alone). crash_install does it for
 * the calling thread; plat_posix.c's thread start calls it for the others. A thread without one dies of an overflow without report.
 *
 * Frame-pointer walks: runtime and lifted code are built -fno-omit-frame-pointer, so x29 chains {previous x29, return address}
 * records up to a thread or fiber entry (fibers start with x29 = 0).
 */
#ifndef SSBU_PLAT_CRASH_H
#define SSBU_PLAT_CRASH_H
#ifndef _WIN32
#include <stdint.h>
#include <stddef.h>

#if defined(__linux__) && defined(__aarch64__)
#define SSBU_ASYNC_CRASH 1          /* hostrt.c / plat_posix.c use this file's handler */
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define CRASH_MAX_FRAMES 64
typedef struct crash_info {
    int sig, code;                  /* signal and si_code */
    uint64_t addr;                  /* si_addr */
    int access;                     /* 'r', 'w' or 0 (not a data abort / not known) */
    uint64_t pc, lr, sp, fp, pstate;
    uint64_t x[31];                 /* x0..x30 of the interrupted code */
    uint64_t pcs[CRASH_MAX_FRAMES]; /* pcs[0] = pc, then the return addresses along the frame chain, innermost first */
    int npcs;
} crash_info;

/* Report text of step 1 (a static buffer; flushed by the handler). Only for use inside a crash_section_fn. */
void crash_put(const char *s);
void crash_put_hex(uint64_t v, int width);          /* lowercase, no "0x", zero-padded to width digits (0: as many as needed) */
void crash_put_dec(long long v);
void crash_put_module(uint64_t addr);               /* "module+0xoff", or "?" */

typedef void (*crash_section_fn)(const crash_info *ci);     /* async-signal-safe; runs once, inside a part */
typedef void (*crash_callback_fn)(const crash_info *ci);    /* step 2: stdio allowed; returns, or ends the process itself */

/* Install the handlers (once; 0 or -errno). report_path may be NULL (console only), is copied. */
int crash_install(const char *report_path, crash_section_fn section, crash_callback_fn callback, int exit_code);
/* Called first on a SIGSEGV / SIGBUS with the fault address; returns 1 when the fault is not a crash (hostrt.c: a first write to a
 * watched page, plat_write_fault). Must be async-signal-safe. */
void crash_set_fault_filter(int (*filter)(void *addr));
void crash_modules_refresh(void);                   /* take the module snapshot again (outside any handler) */
int crash_thread_init(void);                        /* alternate signal stack of the calling thread: 0 or -errno */

/* Never-faulting read of n bytes at addr: 1 if all could be read. Async-signal-safe (needs crash_install's pipe). */
int crash_read(uint64_t addr, void *out, size_t n);
uint64_t crash_strip_pac(uint64_t a);
/* The kind of data abort in an aarch64 Linux sigcontext's __reserved area (esr_context record): 'r', 'w', or 0 when there is no
 * ESR record or it is not a data abort. */
int crash_esr_access(const void *reserved, size_t n);

/* Pieces the handler is made of, exposed for tests/port/crash. */
typedef struct { uint64_t lo, hi, base; char name[40]; } crash_mod;
int crash_mod_find(const crash_mod *tab, int n, uint64_t pc, const crash_mod **out);
/* One line of /proc/self/maps: 1 and the file's base name and offset of pc when the line maps pc. */
int crash_maps_line(const char *line, uint64_t pc, char *name, size_t nsz, uint64_t *off);
int crash_module_of(uint64_t pc, char *name, size_t nsz, uint64_t *off);   /* snapshot, else the maps file */
void crash_set_maps_file(const char *path);         /* default /proc/self/maps */
const char *crash_signal_text(int sig, int code);   /* "SIGSEGV, SEGV_MAPERR: address not mapped" ... */

#ifdef __cplusplus
}
#endif
#endif /* !_WIN32 */
#endif /* SSBU_PLAT_CRASH_H */
