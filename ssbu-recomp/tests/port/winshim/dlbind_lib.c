/* dlbind_lib.c - stands in for ssbu_game*.so in test_modules: a table the executable points into, and the one exported entry
 * point nncxxrt.c eh_bind_main looks up (game_shim_eh.c game_eh_bind). Built -fPIC -fvisibility=hidden. */
const int game_table_stub[4] = {1, 2, 3, 4};
const int *game_table_p(void);
__attribute__((visibility("default"))) const int *game_table_p(void) { return game_table_stub; }
__attribute__((visibility("default"))) int game_eh_bind(int x) { return x + 41; }
