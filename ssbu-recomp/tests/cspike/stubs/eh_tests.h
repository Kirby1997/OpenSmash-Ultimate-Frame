/* placeholder for tests/port/check_host.sh (real file: gen_eh_tests.py) */
typedef struct { uint64_t pc, pad; signed char kind_a, sel_a, kind_b, sel_b; } EhVec;
static const EhVec eh_vecs[] = { {0, 0, 0, 0, 0, 0} };
typedef struct { uint64_t start, end, lsda; } EhFnVec;
static const EhFnVec eh_fn_vecs[] = { {0, 0, 0} };
