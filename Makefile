# Build the recompiled game. The game's code is generated from your copy of the game (see README.md), so `make`
# needs game/analysis/functions.json first (tools/funcs.py).
PY      := tools/.venv/bin/python
CC      := clang
CFLAGS  := -O2 -g -fno-stack-protector -Iruntime/include -Iruntime/src -Iruntime/shaders -Ibuild/gen -Ibuild/shaders
LDFLAGS := -no-pie
LIBS    := -lm -lpng -lz -lSDL3 -lvulkan -lavcodec -lavutil
GLSLC   := glslc
GENFLAGS := $(CFLAGS) -w
# The runtime's own optimisation, on top of CFLAGS: none by default. `make RUNTIME_OPT="-O3 -march=native
# -ffp-contract=off"` builds it for this machine's CPU, vectorised, no multiply-add fused (what it works out for the
# game stays as the PSP's) - about a tenth faster at 144, but it has crashed now and then (not understood yet).
RUNTIME_OPT ?=
# A file linked in, for a build generated with RECOMP_TRACE_ALL=1 (tools/readable/README in docs/readable.md)
TRACE_HOOK ?=
# Coverage of the hand-written code, for what the replays really run of it (docs/readable.md)
HAND_FLAGS ?=
HAND_LDFLAGS ?=

RUNTIME_SRC := $(filter-out runtime/src/main.c,$(wildcard runtime/src/*.c))
RUNTIME_OBJ := $(patsubst runtime/src/%.c,build/runtime/%.o,$(RUNTIME_SRC))
# Game code rewritten by hand, readably (recomp/patches.py HAND)
HAND_SRC := $(wildcard readable/*.c)
RUNTIME_OBJ += $(patsubst readable/%.c,build/hand/%.o,$(HAND_SRC))

all: build/mhfu build/difftest

# Generate the C (one stamp for the whole set of files), then compile each file
build/gen/.stamp: recomp/recomp.py recomp/patches.py tools/modules.py game/analysis/functions.json \
                  $(wildcard tools/readable/names/*.toml)
	$(PY) recomp/recomp.py
	touch $@

GEN_C = $(wildcard build/gen/*.c)
GEN_OBJ = $(patsubst build/gen/%.c,build/obj/%.o,$(GEN_C))

build/obj/%.o: build/gen/%.c build/gen/.stamp runtime/include/recomp.h
	@mkdir -p $(@D)
	@$(CC) $(GENFLAGS) -c $< -o $@

SHADERS := $(patsubst runtime/shaders/%,build/shaders/%.inc,$(wildcard runtime/shaders/*.vert runtime/shaders/*.frag \
                                                                    runtime/shaders/*.tesc runtime/shaders/*.tese))

# Shaders: SPIR-V as C arrays (ge.vert -> ge_vert[])
build/shaders/%.inc: runtime/shaders/% $(wildcard runtime/shaders/*.glsl runtime/shaders/*.h)
	@mkdir -p $(@D)
	$(GLSLC) --target-env=vulkan1.3 -O -mfmt=c $< -o $@.tmp
	printf 'static const uint32_t %s[] = \n' $(subst .,_,$*) > $@ && cat $@.tmp >> $@ && printf ';\n' >> $@ && rm $@.tmp

build/runtime/%.o: runtime/src/%.c $(wildcard runtime/src/*.h runtime/shaders/*.h) runtime/src/nids.inc runtime/include/recomp.h \
                   build/gen/.stamp $(SHADERS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(RUNTIME_OPT) -Wall -Wextra -Wno-missing-field-initializers -c $< -o $@

# The game's structures, offsets checked at compile time: from tools/readable/types.toml and fields/
build/gen/mhfu_types.h: tools/readable/gen_types.py tools/readable/types.toml $(wildcard tools/readable/fields/*.toml) build/gen/.stamp
	$(PY) tools/readable/gen_types.py --header $@

# The debug tools' table of class names, and the build's commit for captures (written only when it changes)
build/gen/class_table.h: tools/readable/gen_class_table.py tools/readable/classes.toml tools/readable/types.toml
	@mkdir -p $(@D)
	$(PY) tools/readable/gen_class_table.py $@

build/gen/git_commit.h: FORCE
	@mkdir -p $(@D)
	@printf '#define MHFU_GIT_COMMIT "%s"\n' "$$(git describe --always --dirty --abbrev=12 2>/dev/null || echo unknown)" > $@.tmp
	@cmp -s $@.tmp $@ && rm -f $@.tmp || mv $@.tmp $@

build/runtime/objects.o: build/gen/class_table.h
build/runtime/capture.o: build/gen/git_commit.h

FORCE:

build/hand/%.o: readable/%.c readable/hand.h build/gen/mhfu_types.h runtime/include/recomp.h build/gen/.stamp
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(HAND_FLAGS) -Ireadable -Wall -Wextra -c $< -o $@

build/game.a: build/gen/.stamp
	@$(MAKE) --no-print-directory $(GEN_OBJ)
	@rm -f $@ && ar rcs $@ $(GEN_OBJ)

build/difftest: runtime/test/difftest.c $(RUNTIME_OBJ) build/game.a
	$(CC) $(CFLAGS) $(LDFLAGS) -Wall -Wextra $< $(RUNTIME_OBJ) build/game.a $(TRACE_HOOK) $(LIBS) -o $@

build/mhfu: build/runtime/main.o $(RUNTIME_OBJ) build/game.a
	$(CC) $(CFLAGS) $(LDFLAGS) $(HAND_LDFLAGS) $^ $(TRACE_HOOK) $(LIBS) -o $@

build/handtest: runtime/test/handtest.c $(RUNTIME_OBJ) build/game.a build/gen/mhfu_types.h
	$(CC) $(CFLAGS) $(LDFLAGS) $(HAND_LDFLAGS) $(HAND_FLAGS) -Wall -Wextra $< $(RUNTIME_OBJ) build/game.a $(TRACE_HOOK) $(LIBS) -o $@

build/vfputest: runtime/test/vfputest.c build/runtime/vfpu.o build/runtime/memory.o build/runtime/dispatch.o build/runtime/cpu.o
	$(CC) $(CFLAGS) -Wall -Wextra $^ -o $@ -Wl,--unresolved-symbols=ignore-all -lm

clean:
	rm -rf build

.PHONY: all clean
