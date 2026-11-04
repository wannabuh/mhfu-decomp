# Build the recompiled game. The game's code is generated from your copy of the game (see README.md), so `make`
# needs game/analysis/functions.json first (tools/funcs.py).
PY      := tools/.venv/bin/python
CC      := clang
# (no stack protector, and the program at a fixed address: a save state holds the threads' host stacks - return
# addresses, and no canary of another run's - so that the same build loads it in another run: runtime/src/savestate.c)
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

RUNTIME_SRC := $(filter-out runtime/src/main.c,$(wildcard runtime/src/*.c))
RUNTIME_OBJ := $(patsubst runtime/src/%.c,build/runtime/%.o,$(RUNTIME_SRC))

all: build/mhfu build/difftest

# Generate the C (one stamp for the whole set of files), then compile each file
build/gen/.stamp: recomp/recomp.py recomp/patches.py tools/modules.py game/analysis/functions.json
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

build/game.a: build/gen/.stamp
	@$(MAKE) --no-print-directory $(GEN_OBJ)
	@rm -f $@ && ar rcs $@ $(GEN_OBJ)

build/difftest: runtime/test/difftest.c $(RUNTIME_OBJ) build/game.a
	$(CC) $(CFLAGS) $(LDFLAGS) -Wall -Wextra $< $(RUNTIME_OBJ) build/game.a $(TRACE_HOOK) $(LIBS) -o $@

build/mhfu: build/runtime/main.o $(RUNTIME_OBJ) build/game.a
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(TRACE_HOOK) $(LIBS) -o $@

build/vfputest: runtime/test/vfputest.c build/runtime/vfpu.o build/runtime/memory.o build/runtime/dispatch.o build/runtime/cpu.o
	$(CC) $(CFLAGS) -Wall -Wextra $^ -o $@ -Wl,--unresolved-symbols=ignore-all -lm

clean:
	rm -rf build

.PHONY: all clean
