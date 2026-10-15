# Build the recompiled game. The game's code is generated from your copy of the game (see README.md), so `make`
# needs game/analysis/functions.json first (tools/funcs.py).
PY      := tools/.venv/bin/python
CC      := clang
CFLAGS  := -O2 -g -Iruntime/include -Iruntime/src -Ibuild/gen -Ibuild/shaders
GLSLC   := glslc
GENFLAGS := $(CFLAGS) -w

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
build/shaders/%.inc: runtime/shaders/% $(wildcard runtime/shaders/*.glsl)
	@mkdir -p $(@D)
	$(GLSLC) --target-env=vulkan1.3 -O -mfmt=c $< -o $@.tmp
	printf 'static const uint32_t %s[] = \n' $(subst .,_,$*) > $@ && cat $@.tmp >> $@ && printf ';\n' >> $@ && rm $@.tmp

build/runtime/%.o: runtime/src/%.c runtime/src/runtime.h runtime/src/hle.h runtime/src/nids.inc runtime/include/recomp.h build/gen/.stamp $(SHADERS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -Wall -Wextra -Wno-missing-field-initializers -c $< -o $@

build/game.a: build/gen/.stamp
	@$(MAKE) --no-print-directory $(GEN_OBJ)
	@rm -f $@ && ar rcs $@ $(GEN_OBJ)

build/difftest: runtime/test/difftest.c $(RUNTIME_OBJ) build/game.a
	$(CC) $(CFLAGS) -Wall -Wextra $< $(RUNTIME_OBJ) build/game.a -lm -lpng -lSDL3 -lvulkan -lavcodec -lavutil -o $@

build/mhfu: build/runtime/main.o $(RUNTIME_OBJ) build/game.a
	$(CC) $(CFLAGS) $^ -lm -lpng -lSDL3 -lvulkan -lavcodec -lavutil -o $@

build/vfputest: runtime/test/vfputest.c build/runtime/vfpu.o build/runtime/memory.o build/runtime/dispatch.o build/runtime/cpu.o
	$(CC) $(CFLAGS) -Wall -Wextra $^ -o $@ -Wl,--unresolved-symbols=ignore-all -lm

clean:
	rm -rf build

.PHONY: all clean
