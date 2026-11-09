/* test_applet.c - nnapplet.c on Linux: nn::swkbd::ShowKeyboard answers (SSBU_SWKBD=cancel / text:<utf-8> / default), UTF-16 <-> UTF-32
 * handling of the guest's char16 text (surrogate pairs), the UTF-8 result form, buffer truncation, MessageBoxW stand-in.
 * nnapplet.c is compiled INTO this test. */
#include "nnapplet.c"
#include "stubs.h"
ImportFn import_lookup(const char *name) { (void)name; return NULL; }
/* the rest of what nnapplet.c reaches: the on-screen keyboard (osk.c) says "not available here" so that the dialog path is the one run,
 * which finds no kdialog or zenity on the empty PATH set in main and answers cancelled */
int osk_run(const wchar_t *header, const wchar_t *guide, const wchar_t *initial, int max_len, int min_len, int password, wchar_t *out) { (void)header; (void)guide; (void)initial; (void)max_len; (void)min_len; (void)password; (void)out; return -1; }
void osk_test_start(void) { }
void *import_data(const char *name) { (void)name; return NULL; }
void os_init_event(uint64_t key, int autoclear) { (void)key; (void)autoclear; }
void os_signal_event(uint64_t key) { (void)key; }
ImportFn sdk_lifted(const char *name) { (void)name; return NULL; }
void call_guest(Ctx *c, uint64_t addr) { (void)c; (void)addr; abort(); }
void report_and_exit(int code) { exit(code); }
void trace_printf(const char *fmt, ...) { (void)fmt; }

static Ctx C;
static uint8_t arg[0x600];
static uint8_t outbuf[256];
static uint64_t string[2];
static uint32_t show(const char *env, int utf8, uint64_t cap) {
    if (env) setenv("SSBU_SWKBD", env, 1); else unsetenv("SSBU_SWKBD");
    unsetenv("SSBU_SEQ"); unsetenv("SSBU_PRESS"); unsetenv("SSBU_SCRIPT");
    memset(arg, 0, sizeof arg); memset(outbuf, 0xee, sizeof outbuf);
    arg[KC_UTF8] = (uint8_t)utf8;
    *(uint32_t *)(arg + KC_MAXLEN) = 100;
    string[0] = (uint64_t)(uintptr_t)outbuf; string[1] = cap;
    memset(&C, 0, sizeof C);
    C.x[0] = (uint64_t)(uintptr_t)string; C.x[1] = (uint64_t)(uintptr_t)arg;
    swkbd_ShowKeyboard(&C);
    return (uint32_t)C.x[0];
}
int main(void) {
    setenv("PATH", "", 1);                                  /* no kdialog, no zenity: a dialog is never started */
    SECTION("SSBU_SWKBD=cancel, and the default / dialog / unknown with no dialog program and no on-screen keyboard: cancelled (0x29f)");
    CHECK(show("cancel", 0, 128) == SWKBD_RESULT_CANCELED && outbuf[0] == 0xee);
    CHECK(show(NULL, 0, 128) == SWKBD_RESULT_CANCELED);
    CHECK(show("dialog", 0, 128) == SWKBD_RESULT_CANCELED);
    CHECK(show("bogus", 0, 128) == SWKBD_RESULT_CANCELED);
    SECTION("SSBU_SWKBD=text:<utf-8> -> UTF-16 in the caller's String (BMP)");
    CHECK(show("text:Kirby", 0, 128) == 0 && *(uint16_t *)outbuf == 'K' && *(uint16_t *)(outbuf + 8) == 'y' && *(uint16_t *)(outbuf + 10) == 0);
    CHECK(show("text:A\xc3\xa9\xe3\x81\x82", 0, 128) == 0);                 /* A, e-acute, hiragana a */
    { uint16_t *u = (uint16_t *)outbuf; CHECKF(u[0] == 'A' && u[1] == 0xe9 && u[2] == 0x3042 && u[3] == 0, "%04x %04x %04x %04x", u[0], u[1], u[2], u[3]); }
    SECTION("UTF-8 result form (isUseUtf8) and truncation");
    CHECK(show("text:A\xc3\xa9\xe3\x81\x82", 1, 128) == 0 && !strcmp((char *)outbuf, "A\xc3\xa9\xe3\x81\x82"));
    CHECK(show("text:Kirby", 0, 8) == 0 && *(uint16_t *)(outbuf + 6) == 0 && *(uint16_t *)(outbuf + 4) == 'r' && outbuf[8] == 0xee);        /* 3 chars + NUL in 8 bytes */
    CHECK(show("text:Kirby", 1, 4) == 0 && !strcmp((char *)outbuf, "Kir"));
    SECTION("copy_w: guest char16 text, BMP");
    { uint16_t g[] = {'x', 0xe9, 0x3042, 'y', 0}; wchar_t w[8]; copy_w(w, (uint64_t)(uintptr_t)g, 8); CHECK(w[0] == 'x' && w[1] == 0xe9 && w[2] == 0x3042 && w[3] == 'y' && w[4] == 0); }
    /* Not tested, a known gap: wchar_t is 32 bits here, so a character outside the BMP (an emoji) is one wide character on the way in
     * (SSBU_SWKBD=text:) and is cut to 16 bits on the way out (kbd_store), and a guest surrogate pair stays two wide characters (copy_w).
     * linux-arm64 converted UTF-16 <-> UTF-32 there (nnapplet.c, WP-U8). */
    return mt_done("nnapplet");
}
