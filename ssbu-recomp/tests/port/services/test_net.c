/* test_net.c - nnnet.c / nnnet_c4a.inc on Linux: the nn::socket service over BSD sockets. nnnet.c is compiled INTO this test. Runs the
 * unit's own c4a_selftest (loopback TCP + UDP exchange through the guest-ABI handlers, error numbers), then nonblocking connect + poll,
 * connection refused, option mapping (BSD numbers -> Linux), and the SSBU_NET=0 offline switch. */
#include "nnnet.c"
#include "stubs.h"
#include <sys/wait.h>
#include <setjmp.h>
#include <signal.h>
static int g_errno_cell;
static void errno_location(Ctx *c) { c->x[0] = (uint64_t)(uintptr_t)&g_errno_cell; }
static void ret0_(Ctx *c) { c->x[0] = 0; }
ImportFn import_lookup(const char *name) {
    if (!strcmp(name, "__errno_location")) return errno_location;
    if (!strncmp(name, "_ZN2nn2os", 9)) return ret0_;             /* mutexes / system events of the ec accessor and ssl parts */
    return svc_find(name);
}
void *import_data(const char *name) { static char slot[64]; (void)name; return slot; }
void guest_trap(const char *why, uint64_t pc) { fprintf(stderr, "guest_trap: %s (pc %llx)\n", why, (unsigned long long)pc); abort(); }
static jmp_buf g_exit_jmp; static int g_exit_armed;
void report_and_exit(int code) { if (g_exit_armed) longjmp(g_exit_jmp, code + 1); exit(code); }
void trace_printf(const char *fmt, ...) { (void)fmt; }
int sdk_image_ready(void) { return 0; }
uint64_t sdk_symbol(const char *n) { (void)n; return 0; }
int sdk_install(const char *n) { (void)n; return 0; }
int account_user_is_open(uint64_t lo, uint64_t hi) { (void)lo; (void)hi; return 1; }
/* the rest of what nnnet.c reaches (the friends / BCAT parts of nnsvc_c5a.inc, online play): none of it runs here */
volatile LONG g_bcat_mounted;
int bcat_dirname_valid(const char *s) { (void)s; return 1; }
void nnldn_register(void) { }
void os_init_event(uint64_t key, int autoclear) { (void)key; (void)autoclear; }
void os_signal_event(uint64_t key) { (void)key; }

#define SO(n) SOK(n)
static Ctx C;
static uint64_t call(const char *name, const uint64_t *a) {
    ImportFn f = svc_find(name);
    if (!f) { fprintf(stderr, "no handler %s\n", name); abort(); }
    memset(&C, 0, sizeof C);
    memcpy(C.x, a, 6 * sizeof *a);
    f(&C);
    return C.x[0];
}
#define CALL(n, ...) call(n, (const uint64_t[6]){__VA_ARGS__})
static int gerrno(void) { return (int)CALL(SO("12GetLastErrnoEv"), 0); }
static void sockaddr_loop(uint8_t sa[16], uint16_t port_host) { memset(sa, 0, 16); sa[0] = 16; sa[1] = 2; sa[2] = port_host >> 8; sa[3] = port_host & 0xff; sa[4] = 127; sa[7] = 1; }

int main(void) {
#ifdef __APPLE__
    signal(SIGPIPE, SIG_IGN);                             /* macOS has no MSG_NOSIGNAL (Linux: nnnet.c adds it to every send) */
#endif
    c4a_register();
    CHECK(svc_nimports > 20);
    static uint8_t pool[0x100000] __attribute__((aligned(4096)));
    uint8_t sa[16], sb[16], buf[64];
    uint32_t len;
    SECTION("the unit's own selftest (loopback TCP and UDP through the handlers, errno table)");
    {   /* c4a_selftest prints one line per group. The resolver-options group needs the SDK image (game data) and the ec group the lifted SDK
         * constructors: those are expected to fail / stop here, every other line (nifm, socket, resolver, ssl, nsd) must be ok. */
        fflush(stdout);
        int saved = dup(1);
        FILE *tf = tmpfile();
        dup2(fileno(tf), 1);
        memset(&C, 0, sizeof C);
        g_exit_armed = 1;
        int bad = 0, stopped = setjmp(g_exit_jmp);
        if (!stopped) bad = c4a_selftest(&C);
        g_exit_armed = 0;
        fflush(stdout); dup2(saved, 1); close(saved);
        rewind(tf);
        char line[512]; int ok_lines = 0, bad_lines = 0, sdk_lines = 0;
        while (fgets(line, sizeof line, tf)) {
            fputs(line, stdout);
            if (strstr(line, ": ok")) ok_lines++;
            else if (strstr(line, "FAILED")) { if (strstr(line, "resolver: options") || strstr(line, "ec:")) sdk_lines++; else bad_lines++; }
        }
        fclose(tf);
        printf("  -> %d groups ok, %d need the SDK image (resolver options / ec), %d failed; stopped=%d\n", ok_lines, sdk_lines, bad_lines, stopped);
        CHECK(bad_lines == 0 && ok_lines >= 17);
        (void)bad;
    }
    SECTION("SSBU_NET unset: online (loopback only); nonblocking connect + Poll(POUT) + SO_ERROR 0; connection refused; BSD option numbers");
    CHECK(CALL(SO("10InitializeEPvmmi"), (uint64_t)(uintptr_t)pool, 0x100000, 0x20000, 4) == 0);
    {
        int ls = (int)CALL(SO("6SocketEiii"), 2, 1, 0);
        CHECK(ls >= 0);
        int one = 1; uint8_t optv[8];
        CHECK((int)CALL(SO("10SetSockOptEiNS0_5LevelENS0_6OptionEPKvj"), ls, 0xffff, 4, (uint64_t)(uintptr_t)&one, 4) == 0);        /* BSD SO_REUSEADDR 4 */
        len = 4; memset(optv, 0xee, 8);
        CHECK((int)CALL(SO("10GetSockOptEiiiPvPj"), ls, 0xffff, 4, (uint64_t)(uintptr_t)optv, (uint64_t)(uintptr_t)&len) == 0 && *(int *)optv != 0);   /* SO_REUSEADDR: 1 on Linux, the flag bit 4 on macOS */
        len = 4; CHECK((int)CALL(SO("10GetSockOptEiiiPvPj"), ls, 0xffff, 0x1008, (uint64_t)(uintptr_t)optv, (uint64_t)(uintptr_t)&len) == 0 && *(int *)optv == 1);   /* SO_TYPE */
        len = 4; CHECK((int)CALL(SO("10GetSockOptEiiiPvPj"), ls, 0xffff, 0x1007, (uint64_t)(uintptr_t)optv, (uint64_t)(uintptr_t)&len) == 0 && *(int *)optv == 0);   /* SO_ERROR */
        int64_t tv[2] = {2, 500000};                                                                                                              /* SO_RCVTIMEO round trip */
        CHECK((int)CALL(SO("10SetSockOptEiNS0_5LevelENS0_6OptionEPKvj"), ls, 0xffff, 0x1006, (uint64_t)(uintptr_t)tv, 16) == 0);
        int64_t back[2] = {0, 0}; len = 16;
        CHECK((int)CALL(SO("10GetSockOptEiiiPvPj"), ls, 0xffff, 0x1006, (uint64_t)(uintptr_t)back, (uint64_t)(uintptr_t)&len) == 0 && back[0] == 2 && back[1] == 500000 && len == 16);
        CHECK((int)CALL(SO("10SetSockOptEiNS0_5LevelENS0_6OptionEPKvj"), ls, 0xffff, 0x7777, (uint64_t)(uintptr_t)&one, 4) == -1 && gerrno() == 92);            /* unknown option */
        sockaddr_loop(sa, 0);
        CHECK((int)CALL(SO("4BindEiPK8sockaddrj"), ls, (uint64_t)(uintptr_t)sa, 16) == 0);
        CHECK((int)CALL(SO("6ListenEii"), ls, 4) == 0);
        len = 16; memset(sb, 0, 16);
        CHECK((int)CALL("nnsocketGetSockName", ls, (uint64_t)(uintptr_t)sb, (uint64_t)(uintptr_t)&len) == 0);
        /* bound_any reports 0.0.0.0, the port is the real one: ask the kernel */
        struct sockaddr_in real; socklen_t rl = sizeof real; getsockname(g_so_fd[ls].o->s, (struct sockaddr *)&real, &rl);
        uint16_t port = ntohs(real.sin_port);
        CHECK(port != 0);
        sockaddr_loop(sa, port);
        int cs = (int)CALL(SO("6SocketEiii"), 2, 1, 0);
        CHECK((int)CALL(SO("5FcntlEiiz"), cs, 4, 0x800) == 0);                                                                                    /* O_NONBLOCK */
        CHECK((int)CALL(SO("5FcntlEiiz"), cs, 3, 0) == 0x800);
        int r = (int)CALL(SO("7ConnectEiPK8sockaddrj"), cs, (uint64_t)(uintptr_t)sa, 16);
        CHECKF(r == 0 || (r == -1 && gerrno() == 115), "nonblocking connect: %d errno %d", r, gerrno());
        uint8_t pfd[8] = {0}; *(int *)pfd = cs; *(uint16_t *)(pfd + 4) = 4;                                                                       /* POLLOUT */
        r = (int)CALL(SO("4PollEP6pollfdmi"), (uint64_t)(uintptr_t)pfd, 1, 2000);
        CHECKF(r == 1 && (*(uint16_t *)(pfd + 6) & 4), "poll for connect: %d revents %x", r, *(uint16_t *)(pfd + 6));
        len = 4; CHECK((int)CALL(SO("10GetSockOptEiiiPvPj"), cs, 0xffff, 0x1007, (uint64_t)(uintptr_t)optv, (uint64_t)(uintptr_t)&len) == 0 && *(int *)optv == 0);
        r = (int)CALL(SO("6AcceptEiP8sockaddrPj"), ls, 0, 0);
        CHECK(r >= 0);
        CHECK((int64_t)CALL(SO("4SendEiPKvmi"), cs, (uint64_t)(uintptr_t)"hello", 5, 0) == 5);
        *(int *)pfd = r; *(uint16_t *)(pfd + 4) = 1; *(uint16_t *)(pfd + 6) = 0;
        CHECK((int)CALL(SO("4PollEP6pollfdmi"), (uint64_t)(uintptr_t)pfd, 1, 2000) == 1 && (*(uint16_t *)(pfd + 6) & 1));
        memset(buf, 0, sizeof buf);
        CHECK((int64_t)CALL(SO("4RecvEiPvmi"), r, (uint64_t)(uintptr_t)buf, sizeof buf, 0) == 5 && !memcmp(buf, "hello", 5));
        CHECK((int64_t)CALL(SO("4RecvEiPvmi"), r, (uint64_t)(uintptr_t)buf, sizeof buf, 0x80) == -1 && gerrno() == 11);                          /* MSG_DONTWAIT: EAGAIN */
        /* peer closed: send to it fails with EPIPE (32), not SIGPIPE */
        CHECK((int)CALL(SO("5CloseEi"), r) == 0);
        (void)CALL(SO("4SendEiPKvmi"), cs, (uint64_t)(uintptr_t)"x", 1, 0);
        usleep(50000);
        int64_t sr = (int64_t)CALL(SO("4SendEiPKvmi"), cs, (uint64_t)(uintptr_t)"y", 1, 0);
        CHECKF(sr == -1 && (gerrno() == 32 || gerrno() == 104), "send after peer close: %lld errno %d", (long long)sr, gerrno());
        CHECK((int)CALL(SO("5CloseEi"), cs) == 0);
        /* connection refused: the listener's port, closed */
        CHECK((int)CALL(SO("5CloseEi"), ls) == 0);
        cs = (int)CALL(SO("6SocketEiii"), 2, 1, 0);
        r = (int)CALL(SO("7ConnectEiPK8sockaddrj"), cs, (uint64_t)(uintptr_t)sa, 16);
        CHECKF(r == -1 && gerrno() == 111, "refused: %d errno %d", r, gerrno());
        /* non-loopback: no network */
        sa[4] = 8; sa[5] = 8; sa[6] = 8; sa[7] = 8;
        r = (int)CALL(SO("7ConnectEiPK8sockaddrj"), cs, (uint64_t)(uintptr_t)sa, 16);
        CHECKF(r == -1 && gerrno() == 101, "no network: %d errno %d", r, gerrno());
        CHECK((int)CALL(SO("5CloseEi"), cs) == 0);
        /* UDP round trip + truncated datagram */
        int u1 = (int)CALL(SO("6SocketEiii"), 2, 2, 0), u2 = (int)CALL(SO("6SocketEiii"), 2, 2, 0);
        sockaddr_loop(sa, 0);
        CHECK((int)CALL(SO("4BindEiPK8sockaddrj"), u1, (uint64_t)(uintptr_t)sa, 16) == 0);
        struct sockaddr_in ur; rl = sizeof ur; getsockname(g_so_fd[u1].o->s, (struct sockaddr *)&ur, &rl);
        sockaddr_loop(sa, ntohs(ur.sin_port));
        CHECK((int64_t)CALL(SO("6SendToEiPKvmiPK8sockaddrj"), u2, (uint64_t)(uintptr_t)"datagram!", 9, 0, (uint64_t)(uintptr_t)sa, 16) == 9);
        len = 16;
        CHECK((int64_t)CALL(SO("8RecvFromEiPvmiP8sockaddrPj"), u1, (uint64_t)(uintptr_t)buf, 4, 0, (uint64_t)(uintptr_t)sb, (uint64_t)(uintptr_t)&len) == 4 && !memcmp(buf, "data", 4) && sb[1] == 2 && sb[4] == 127);
        CHECK((int)CALL(SO("5CloseEi"), u1) == 0 && (int)CALL(SO("5CloseEi"), u2) == 0);
    }
    CHECK((int)CALL(SO("8FinalizeEv"), 0) == 0);
    /* Not here: linux-arm64's SSBU_NET=0 switch (socket() fails with ENETUNREACH, an offline first boot); the rebuilt layer has
     * upstream's offline model instead (nifm reports no connection, sockets still open: loopback only). */
    return mt_done("nnnet");
}
