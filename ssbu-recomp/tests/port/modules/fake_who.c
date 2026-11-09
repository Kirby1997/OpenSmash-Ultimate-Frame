/* the same function name in every fake module (and in the test executable, exported with -rdynamic): which copy a
 * module's call reaches shows whether it is bound to itself (-fvisibility=hidden / -Bsymbolic) or interposed */
int fake_whoami(void);
int fake_whoami(void) { return WHO; }
