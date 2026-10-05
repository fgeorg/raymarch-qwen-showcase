# raymarch-qwen-showcase
#
#   make            build the GPU (Vulkan compute) renderer
#   make run        render one 1024^2 frame -> out/render.png (GPU, timed with
#                   VK_QUERY_TYPE_TIMESTAMP)
#   make video      150-frame clip (10 s @ 15 fps): 7 s camera pan drops from
#                   overhead to a level, sun-backlit shot, then holds the
#                   final pose for the last 3 s -> out/showcase.mp4
#                   (frames are intermediate and deleted after encoding;
#                   `make frames` keeps them; `make video RES=512` for 512 res)
#   make preview    512px-wide 8-bit JPEG of a big PNG for cheap visual
#                   inspection -> out/preview.jpg (`make preview IMG=...`)
#   make frames     render the 150 clip frames to out/frames (no encoding)
#   make diff       100%-diffuse debug render of frame 100 -> out/diff.png
#                   (all materials forced Lambert; banding triage)
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

.PHONY: all run video frames preview diag diff flatsky flatfloor flatboth hisamp normview mirrorview frame100 clean
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
# DIFF: additive debug pass (no changes to main scene behavior). Renders
# frame 100 with -DDIFF: every material forced to 100% diffuse, so any
# banding that survives is in the diffuse sky integration / tonemap, not
# the specular BSDF. Clobbers out/render.png (make run restores it).
diff: GPU_FLAGS=-DDIFF
diff: $(BIN_GPU)
	$(GPU_ENV) ./$(BIN_GPU) --frame 100 --out out && mv out/render.png out/diff.png

# Banding triage renders (frame 100, each clobbers out/render.png; make run
# restores it):
#   flatsky   -DFLATSKY        uniform sky -> isolates sky-gradient banding
#   flatfloor -DFLATFLOOR      no checker  -> isolates checker moire in gloss
#   flatboth  both flat        clean -> sky+floor cause; banded -> BSDF/surface
#   hisamp    -DAA_SAMPLES=512 band amplitude drops -> variance; unchanged ->
#                              deterministic (precision/quantization)
flatsky: GPU_FLAGS=-DFLATSKY
flatsky: $(BIN_GPU)
	$(GPU_ENV) ./$(BIN_GPU) --frame 100 --out out && mv out/render.png out/flatsky.png
flatfloor: GPU_FLAGS=-DFLATFLOOR
flatfloor: $(BIN_GPU)
	$(GPU_ENV) ./$(BIN_GPU) --frame 100 --out out && mv out/render.png out/flatfloor.png
flatboth: GPU_FLAGS=-DFLATSKY -DFLATFLOOR
flatboth: $(BIN_GPU)
	$(GPU_ENV) ./$(BIN_GPU) --frame 100 --out out && mv out/render.png out/flatboth.png
hisamp: GPU_FLAGS=-DAA_SAMPLES=512
hisamp: $(BIN_GPU)
	$(GPU_ENV) ./$(BIN_GPU) --frame 100 --out out && mv out/render.png out/hisamp.png

# Sampling-vs-geometry triage (frame 100, single center ray per pixel, no AA
# loop; each clobbers out/render.png; make run restores it):
#   normview    -DNORMVIEW     first-hit normal per pixel (sky black); rings
#                              here => SDF/normal quantization, not sampling
#   mirrorview  -DMIRRORVIEW   fuzzReflect direction at u=0.5 per pixel
normview: GPU_FLAGS=-DNORMVIEW
normview: $(BIN_GPU)
	$(GPU_ENV) ./$(BIN_GPU) --frame 100 --out out && mv out/render.png out/normview.png
mirrorview: GPU_FLAGS=-DMIRRORVIEW
mirrorview: $(BIN_GPU)
	$(GPU_ENV) ./$(BIN_GPU) --frame 100 --out out && mv out/render.png out/mirrorview.png

# Production render of one clip frame (default AA, no debug defines):
#   make frame100 -> out/frame100.png (banding baseline is frame 100)
frame100: $(BIN_GPU)
	$(GPU_ENV) ./$(BIN_GPU) --frame 100 --out out && mv out/render.png out/frame100.png
diag: $(BIN_GPU)
	$(GPU_ENV) ./$(BIN_GPU) --frame 100 --out out && mv out/render.png out/diag_100.png
	$(GPU_ENV) ./$(BIN_GPU) --frame 149 --out out && mv out/render.png out/diag_149.png
	python3 tools/diag_check.py out/diag_100.png 100
	python3 tools/diag_check.py out/diag_149.png 149

clean:
	rm -rf build out
