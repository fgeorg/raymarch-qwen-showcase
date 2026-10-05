# raymarch-qwen-showcase
#
#   make            build the GPU (Vulkan compute) renderer
#   make run        render one 1024^2 frame -> out/render.png (GPU, timed with
#                   VK_QUERY_TYPE_TIMESTAMP)
#   make video      150-frame clip (10 s @ 15 fps): camera drops from
#                   overhead to a level, sun-backlit shot -> out/showcase.mp4
#                   (frames are intermediate and deleted after encoding;
#                   `make frames` keeps them; `make video RES=512` for 512 res)
#   make preview    512px-wide 8-bit JPEG of a big PNG for cheap visual
#                   inspection -> out/preview.jpg (`make preview IMG=...`)
#   make frames     render the 150 clip frames to out/frames (no encoding)
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
CXXFLAGS ?= -std=c++17 -O3 -march=native -flto -Wall -Wextra -Wno-missing-field-initializers -pthread
GLSLANG  ?= glslangValidator
GPU_FLAGS ?=
FZZ     ?= 1.0   # runtime fuzz: make run FZZ=2 (0.5 crisp .. 2.0 fuzzy)
RES     ?= 1024  # frame size for clip frames: make video RES=512
LDLIBS   := -lz -lpthread

BIN_GPU  := build/vkmain
SPV      := build/scene.spv

.PHONY: all run video frames preview diag clean
all: $(BIN_GPU)

# GPU_FLAGS must trigger an SPV rebuild even when scene.comp is untouched
# (plain make only compares timestamps -> old -D defines silently survived).
FLAGS_STAMP := build/gpuflags.stamp
FORCE:
$(FLAGS_STAMP): FORCE
	@if [ "$$(cat $@ 2>/dev/null)" != "$(GPU_FLAGS)" ]; then \
	  rm -f $(SPV); echo "$(GPU_FLAGS)" > $@; \
	fi

$(SPV): src/scene.comp src/diag.inc $(FLAGS_STAMP) | dirs
	$(GLSLANG) -V -S comp -e main -Isrc $(GPU_FLAGS) --target-env vulkan1.2 -o $@ $<

$(BIN_GPU): src/vkmain.cpp src/png.h $(SPV) | dirs
	$(CXX) $(CXXFLAGS) src/vkmain.cpp -o $@ -lvulkan $(LDLIBS)

dirs:
	mkdir -p build out

run: $(BIN_GPU)
	$(GPU_ENV) ./$(BIN_GPU) --fuzz $(FZZ)

# Render the clip frames and keep them (no encode)
frames: $(BIN_GPU)
	mkdir -p out/frames
	$(GPU_ENV) ./$(BIN_GPU) --frames 150 --res $(RES) --out out/frames

video: $(BIN_GPU)
	mkdir -p out/frames
	$(GPU_ENV) ./$(BIN_GPU) --frames 150 --res $(RES) --out out/frames
	ffmpeg -y -framerate 15 -i out/frames/%04d.png -c:v libx264 -pix_fmt yuv420p out/showcase.mp4
	rm -rf out/frames

# Small 8-bit JPEG (512px wide) for cheap visual inspection of a big 16-bit
# PNG: make preview [IMG=out/frames/0050.png]
IMG ?= out/render.png
preview:
	magick $(IMG) -resize 512x -quality 85 out/preview.jpg

# DIAG: additive validation pass (no changes to main scene behavior).
# Renders two frames with -DDIAG: left half = production fuzzReflect BSDF
# (48 samples), right half = exact mirror; CPU checker validates both
# against analytic ground truth. Clobbers out/render.png (make run restores it).
diag: GPU_FLAGS=-DDIAG -DAA_SAMPLES=16
diag: $(BIN_GPU)
	$(GPU_ENV) ./$(BIN_GPU) --frame 100 --out out && mv out/render.png out/diag_100.png
	$(GPU_ENV) ./$(BIN_GPU) --frame 149 --out out && mv out/render.png out/diag_149.png
	python3 tools/diag_check.py out/diag_100.png 100
	python3 tools/diag_check.py out/diag_149.png 149

clean:
	rm -rf build out
