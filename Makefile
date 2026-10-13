# Build the recompiled game. The game's code is generated from your copy of the game (see README.md), so `make`
# needs game/analysis/functions.json first (tools/funcs.py).
PY      := tools/.venv/bin/python
CC      := clang
CFLAGS  := -O2 -g -Iruntime/include -Iruntime/src -Ibuild/gen
GENFLAGS := $(CFLAGS) -w

RUNTIME_SRC := $(filter-out runtime/src/main.c,$(wildcard runtime/src/*.c))
RUNTIME_OBJ := $(patsubst runtime/src/%.c,build/runtime/%.o,$(RUNTIME_SRC))

all: build/mhfu build/difftest

# Generate the C (one stamp for the whole set of files), then compile each file
build/gen/.stamp: recomp/recomp.py tools/modules.py game/analysis/functions.json
	$(PY) recomp/recomp.py
	touch $@

GEN_C = $(wildcard build/gen/*.c)
GEN_OBJ = $(patsubst build/gen/%.c,build/obj/%.o,$(GEN_C))

build/obj/%.o: build/gen/%.c build/gen/.stamp runtime/include/recomp.h
	@mkdir -p $(@D)
	@$(CC) $(GENFLAGS) -c $< -o $@

build/runtime/%.o: runtime/src/%.c runtime/src/runtime.h runtime/src/hle.h runtime/src/nids.inc runtime/include/recomp.h build/gen/.stamp
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -Wall -Wextra -c $< -o $@

build/game.a: build/gen/.stamp
	@$(MAKE) --no-print-directory $(GEN_OBJ)
	@rm -f $@ && ar rcs $@ $(GEN_OBJ)

build/difftest: runtime/test/difftest.c $(RUNTIME_OBJ) build/game.a
	$(CC) $(CFLAGS) -Wall -Wextra $< $(RUNTIME_OBJ) build/game.a -lm -lpng -o $@

build/mhfu: build/runtime/main.o $(RUNTIME_OBJ) build/game.a
	$(CC) $(CFLAGS) $^ -lm -lpng -o $@

clean:
	rm -rf build

.PHONY: all clean
