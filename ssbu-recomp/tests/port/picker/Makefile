# tests/port/picker - the in-game game-data picker (WP-U5b).
#   make [CC=clang] [SAN=1]        test_picker: runtime/datapicker.c + datapaths.c without a display (any Linux host, also WSL)
#   make sdl [SDL=<SDL3 prefix>]   test_picker_sdl: + datapicker_sdl.c in a real SDL3 window (Steam Frame: run_frame.sh runs it
#                                  with SDL's offscreen driver and on the gamescope session's display)
REPO    ?= ../../..
RT      := $(REPO)/runtime
CC      ?= cc
SAN     ?=
SDL     ?= $(HOME)/ssbu-port/deps/sdl3
BUILD   := build/$(notdir $(CC))$(if $(SAN),-san)
SCRATCH ?= $(CURDIR)/$(BUILD)
COMMA   := ,
SANFL   := $(if $(SAN),-fsanitize=address$(COMMA)undefined -fno-sanitize-recover=undefined)
WARN    := -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers
CFLAGS  := -O1 -g -std=gnu11 -D_GNU_SOURCE -fno-pie -fno-omit-frame-pointer -fno-strict-aliasing -pthread \
           -include $(RT)/plat/plat_pre.h -I$(RT)/plat -I$(RT) -I$(REPO) -I$(REPO)/tests/port/boot -I$(SDL)/include -idirafter $(REPO)/tests/port/threads/stub -I. $(SANFL) \
           -Dfn_table='(*game_fn_table_p)' -Dfn_table_count='(*game_fn_table_count_p)'
LDFLAGS := -no-pie -pthread -rdynamic -ldl -lm -Wl,--unresolved-symbols=ignore-all $(SANFL)
# as tests/port/boot/run.sh: the host's platform layer (plat_posix.c, which also holds the host_* path helpers) and datapaths.c;
# whatever else plat_posix.c reaches for (vmem, crash handler) is not used by the picker and is left unresolved
OBJS    := $(addprefix $(BUILD)/,plat_posix.o datapaths.o)

all: run
build: $(BUILD)/test_picker
run: build
	@echo "== $(BUILD)/test_picker"; SSBU_VMEM_NO_REEXEC=1 ./$(BUILD)/test_picker "$(SCRATCH)" "$(SCRATCH)/shots"
sdl: $(BUILD)/test_picker_sdl

$(BUILD):
	mkdir -p $@
# the files of this work package are held to -Wall -Wextra -Werror; the rest is compiled as the game build does
$(BUILD)/datapaths.o: $(RT)/datapaths.c $(RT)/datapaths.h $(RT)/hostpath.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) -Werror -c $< -o $@
$(BUILD)/datapicker_sdl.o: $(RT)/datapicker_sdl.c $(RT)/datapicker.h $(RT)/datapaths.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) -Werror -c $< -o $@
$(BUILD)/datapicker.o: $(RT)/datapicker.c $(RT)/datapicker.h $(RT)/datapaths.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) -Werror -c $< -o $@
$(BUILD)/%.o: $(RT)/%.c $(wildcard $(RT)/*.h) | $(BUILD)
	$(CC) $(CFLAGS) -w -c $< -o $@
$(BUILD)/%.o: $(RT)/%.S | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
# test_picker includes datapicker.c (its static helpers are checked too)
$(BUILD)/test_picker: test_picker.c $(RT)/datapicker.c $(RT)/datapicker.h $(OBJS)
	$(CC) $(CFLAGS) $(WARN) -Werror $< $(OBJS) $(LDFLAGS) -o $@
$(BUILD)/test_picker_sdl: test_picker_sdl.c $(BUILD)/datapicker.o $(BUILD)/datapicker_sdl.o $(OBJS)
	$(CC) $(CFLAGS) $(WARN) -Werror $< $(BUILD)/datapicker.o $(BUILD)/datapicker_sdl.o $(OBJS) \
	    -L$(SDL)/lib -lSDL3 -Wl,-rpath,$(SDL)/lib $(LDFLAGS) -o $@
clean:
	rm -rf build
.PHONY: all build run sdl clean
