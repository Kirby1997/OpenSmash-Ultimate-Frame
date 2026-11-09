/* test_misc.c - nnmisc.c's nn::oe::GetDesiredLanguage on the locale of the host: $LANGUAGE / $LC_ALL / $LC_MESSAGES / $LANG (plat's
 * GetUserPreferredUILanguages) -> one of the 14 language codes the console knows, plus the SSBU_LANGUAGE override and the order of the
 * variables. desired_language() caches its first answer, so every case runs in a child process. nnmisc.c is compiled INTO this test.
 * The locale half of linux-arm64's tests/port/misc/test_misc.c (the SHA-1 / MD5 / getrandom half is tests/port/winshim/test_crypto.c).
 * Differences from linux-arm64's table, as upstream's mapping has them: pt_BR and the like give en-US (no match) and zh_SG gives zh-Hans. */
#include "nnmisc.c"
#include "stubs.h"
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>

/* what nnmisc.c reaches in the rest of the runtime (nothing of it runs here) */
const char *gfx_language(void) { return ""; }
void guest_trap(const char *why, uint64_t pc) { fprintf(stderr, "guest_trap: %s (pc %llx)\n", why, (unsigned long long)pc); abort(); }
void *import_data(const char *name) { (void)name; return NULL; }
void report_and_exit(int code) { exit(code); }
void trace_printf(const char *fmt, ...) { (void)fmt; }
void os_init_event(uint64_t key, int autoclear) { (void)key; (void)autoclear; }
void os_signal_event(uint64_t key) { (void)key; }
void call_guest(Ctx *c, uint64_t addr) { (void)c; (void)addr; abort(); }
void call_indirect(Ctx *c, uint64_t addr) { (void)c; (void)addr; abort(); }
int host_fixed_random(void *buffer, uint64_t size, const char *who, uint64_t lr) { (void)buffer; (void)size; (void)who; (void)lr; return 0; }
ImportFn import_lookup(const char *name) { (void)name; return NULL; }
volatile LONG g_guest_frozen;
int g_game_build;
MainLayout g_main;
static const DynSym no_syms[1];
static const int zero;
const DynSym (*game_dyn_syms_p)[] = (const DynSym (*)[])&no_syms;
const int *game_dyn_sym_count_p = &zero;
uint64_t hostfn_address(const char *name, ImportFn fn) { (void)name; return (uint64_t)(uintptr_t)fn; }
uint64_t ga(uint64_t a) { return a; }

static void ask(const char *lang_all, const char *lang_messages, const char *lang, const char *language, const char *ssbu, char out[16]) {
    int fd[2];
    if (pipe(fd)) abort();
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        const char *e[5][2] = {{"LC_ALL", lang_all}, {"LC_MESSAGES", lang_messages}, {"LANG", lang}, {"LANGUAGE", language}, {"SSBU_LANGUAGE", ssbu}};
        for (int i = 0; i < 5; i++) { if (e[i][1]) setenv(e[i][0], e[i][1], 1); else unsetenv(e[i][0]); }
        int dn = open("/dev/null", O_WRONLY);
        dup2(dn, 1);                                          /* its "GetDesiredLanguage: ..." line */
        uint64_t v = desired_language();
        char s[16] = {0};
        memcpy(s, &v, 8);
        if (write(fd[1], s, 16) != 16) _exit(1);
        _exit(0);
    }
    close(fd[1]);
    memset(out, 0, 16);
    if (read(fd[0], out, 16) != 16) strcpy(out, "(child failed)");
    close(fd[0]);
    int st;
    waitpid(pid, &st, 0);
}
static void lang(const char *loc, const char *want) {
    char got[16];
    ask(NULL, NULL, loc, NULL, NULL, got);
    CHECKF(!strcmp(got, want), "$LANG \"%s\" -> \"%s\", want \"%s\"", loc ? loc : "(unset)", got, want);
}

int main(void) {
    SECTION("$LANG values -> the console's language codes");
    lang("en_US.UTF-8", "en-US"); lang("en_GB.UTF-8", "en-GB"); lang("en_AU.UTF-8", "en-GB"); lang("en_CA.UTF-8", "en-US");
    lang("de_DE.UTF-8", "de"); lang("de_DE@euro", "de"); lang("fr_FR.UTF-8", "fr"); lang("fr_CA.UTF-8", "fr-CA"); lang("fr_BE", "fr");
    lang("es_ES.UTF-8", "es"); lang("es_MX.UTF-8", "es-419"); lang("es_AR", "es-419"); lang("it_IT.UTF-8", "it"); lang("ja_JP.UTF-8", "ja");
    lang("ko_KR.UTF-8", "ko"); lang("nl_NL.UTF-8", "nl"); lang("ru_RU.UTF-8", "ru");
    lang("zh_CN.UTF-8", "zh-Hans"); lang("zh_TW.UTF-8", "zh-Hant"); lang("zh_HK.UTF-8", "zh-Hant"); lang("zh_SG", "zh-Hans");
    lang("pt_BR.UTF-8", "en-US"); lang("C", "en-US"); lang("POSIX", "en-US"); lang("C.UTF-8", "en-US"); lang("", "en-US"); lang("x", "en-US");
    SECTION("order: $LANGUAGE, then $LC_ALL, $LC_MESSAGES, $LANG (plat's locale_tag); SSBU_LANGUAGE first of all");
    char got[16];
    ask("ko_KR.UTF-8", "ja_JP.UTF-8", "de_DE.UTF-8", NULL, NULL, got);     CHECKF(!strcmp(got, "ko"), "LC_ALL wins: %s", got);
    ask(NULL, "ja_JP.UTF-8", "de_DE.UTF-8", NULL, NULL, got);              CHECKF(!strcmp(got, "ja"), "then LC_MESSAGES: %s", got);
    ask("", "", "de_DE.UTF-8", NULL, NULL, got);                           CHECKF(!strcmp(got, "de"), "empty values are skipped: %s", got);
    ask("ko_KR.UTF-8", NULL, "de_DE.UTF-8", "fr_FR:en_US", NULL, got);     CHECKF(!strcmp(got, "fr"), "LANGUAGE first, its first entry: %s", got);
    ask("ko_KR.UTF-8", NULL, "de_DE.UTF-8", NULL, "it", got);              CHECKF(!strcmp(got, "it"), "SSBU_LANGUAGE over all: %s", got);
    ask(NULL, NULL, NULL, NULL, NULL, got);                                CHECKF(!strcmp(got, "en-US"), "nothing set: %s", got);
    return mt_done("nnmisc");
}
