# raymarch-qwen-showcase
#
#   make            build the GPU (Vulkan compute) renderer  + CPU fallback
#   make run        render one 1024^2 frame -> out/render.png  (GPU, timed with
#                   VK_QUERY_TYPE_TIMESTAMP; falls back to CPU if no Vulkan)
#   make video      300-frame orbit -> out/frames/%04d.png -> out/showcase.mp4
#   make cpu        build the CPU raymarcher only
#   make clean      remove build artifacts
#
# Quality knobs are #defines in src/scene.comp, e.g.:
#   make GPU_FLAGS="-DAA_SAMPLES=4 -DSPEC_SAMPLES=4"
#
# The locally-built Mesa 26.2 (system Mesa crashes on vkCreateComputePipelines)
# is used automatically when present:
VK_ICD_FILES := /tmp/mesa262/icds/intel.json:/tmp/mesa262/icds/lvp.json
HAVE_LOCAL_MESA := $(shell [ -f /tmp/mesa262/icds/intel.json ] && echo yes)
GPU_ENV := $(if $(filter yes,$(HAVE_LOCAL_MESA)),VK_ICD_FILENAMES=$(VK_ICD_FILES))

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O3 -march=native -flto -Wall -Wextra -pthread
GLSLANG  ?= glslangValidator
GPU_FLAGS ?=
FZZ     ?= 1.0   # runtime fuzz: make run FZZ=2 (0.5 crisp .. 2.0 fuzzy)
LDLIBS   := -lz -lpthread

BIN_GPU  := build/vkmain
BIN_CPU  := build/raymarch
SPV      := build/scene.spv
PNG      := out/render.png

.PHONY: all run video cpu clean
all: $(BIN_GPU) $(BIN_CPU)

# GPU_FLAGS must trigger an SPV rebuild even when scene.comp is untouched
# (plain make only compares timestamps -> old -D defines silently survived).
FLAGS_STAMP := build/gpuflags.stamp
FORCE:
$(FLAGS_STAMP): FORCE
	@if [ "$$(cat $@ 2>/dev/null)" != "$(GPU_FLAGS)" ]; then \
	  rm -f $(SPV); echo "$(GPU_FLAGS)" > $@; \
	fi

$(SPV): src/scene.comp $(FLAGS_STAMP) | dirs
	$(GLSLANG) -V -S comp -e main $(GPU_FLAGS) --target-env vulkan1.2 -o $@ $<

$(BIN_GPU): src/vkmain.cpp src/png.h $(SPV) | dirs
	$(CXX) $(CXXFLAGS) src/vkmain.cpp -o $@ -lvulkan $(LDLIBS)

$(BIN_CPU): src/raymarch.cpp src/png.h | dirs
	$(CXX) $(CXXFLAGS) src/raymarch.cpp -o $@ $(LDLIBS)

dirs:
	mkdir -p build out

run: $(BIN_GPU) $(BIN_CPU)
	{ $(GPU_ENV) ./$(BIN_GPU) --fuzz $(FZZ); } || { echo "GPU failed, falling back to CPU"; ./$(BIN_CPU); }

video: $(BIN_GPU)
	mkdir -p out/frames
	$(GPU_ENV) ./$(BIN_GPU) --frames 300 --out out/frames
	ffmpeg -y -framerate 30 -i out/frames/%04d.png -c:v libx264 -pix_fmt yuv420p out/showcase.mp4

cpu: $(BIN_CPU)
	./$(BIN_CPU)

clean:
	rm -rf build out
