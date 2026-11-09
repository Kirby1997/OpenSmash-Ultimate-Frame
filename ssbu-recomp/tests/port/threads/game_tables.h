/* game_tables.h - forced into the units a test builds that reach the game's tables (fn_table, dyn_syms: nncxxrt.c, nncxxlib.c,
 * nnlibc2.c, clib.c, stubs_cxxrt.c) when there is no game library: the tables are reached through pointers that stubs_cxxrt.c
 * points at empty ones, as the game library's shims do (game_shim.c). */
#ifndef TEST_GAME_TABLES_H
#define TEST_GAME_TABLES_H
#define fn_table (*game_fn_table_p)
#define fn_table_count (*game_fn_table_count_p)
#define dyn_syms (*game_dyn_syms_p)
#define dyn_sym_count (*game_dyn_sym_count_p)
#endif
