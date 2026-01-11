# ffrog - Makefile wrapper around CMake
# Default: portable Release build using clang++ + lld + ThinLTO (no -march=native).

# ---- User-tunable knobs -------------------------------------------------
BUILD_DIR   ?= build
CMAKE_GEN   ?= Unix Makefiles
BUILD_TYPE  ?= Release

override CC := clang
override CXX := clang++

# 1 = link with lld (recommended). Set to 0 to use the system default linker.
USE_LLD     ?= 1

# LTO modes: off | thin | full
LTO         ?= thin

# Extra flags you may pass from the command line, e.g.:
#   make EXTRA_CXXFLAGS='-g0' EXTRA_LDFLAGS='-Wl,--verbose'
EXTRA_CFLAGS    ?=
EXTRA_CXXFLAGS  ?=
EXTRA_LDFLAGS   ?=

# ---- Internals ----------------------------------------------------------
NPROC := $(shell nproc 2>/dev/null || echo 4)

CFLAGS_RELEASE   := -O3 -DNDEBUG -pipe -ffunction-sections -fdata-sections $(EXTRA_CFLAGS)
CXXFLAGS_RELEASE := -O3 -DNDEBUG -pipe -ffunction-sections -fdata-sections $(EXTRA_CXXFLAGS)

LDFLAGS_COMMON := -Wl,--gc-sections -Wl,-O2 -Wl,--as-needed $(EXTRA_LDFLAGS)

ifeq ($(USE_LLD),1)
  LDFLAGS_COMMON += -fuse-ld=lld
endif

ifeq ($(LTO),thin)
  CFLAGS_RELEASE   += -flto=thin
  CXXFLAGS_RELEASE += -flto=thin
  # lld understands this; if ignored, it’s harmless.
  LDFLAGS_COMMON   += -Wl,--thinlto-jobs=$(NPROC)
else ifeq ($(LTO),full)
  CFLAGS_RELEASE   += -flto
  CXXFLAGS_RELEASE += -flto
endif

CMAKE_ARGS := \
  -G "$(CMAKE_GEN)" \
  -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
  -DCMAKE_C_COMPILER=$(CC) \
  -DCMAKE_CXX_COMPILER=$(CXX) \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_C_FLAGS_RELEASE="$(CFLAGS_RELEASE)" \
  -DCMAKE_CXX_FLAGS_RELEASE="$(CXXFLAGS_RELEASE)" \
  -DCMAKE_EXE_LINKER_FLAGS="$(LDFLAGS_COMMON)"

# ---- Targets ------------------------------------------------------------
.PHONY: all build clean configure distclean doctor help install reconfigure run

all: build

$(BUILD_DIR)/.created:
	@mkdir -p "$(BUILD_DIR)"
	@touch "$@"

doctor:
	@echo "CC=$(CC)"
	@echo "CXX=$(CXX)"
	@command -v "$(CC)" >/dev/null 2>&1 || (echo "Missing compiler: $(CC)"; exit 1)
	@command -v "$(CXX)" >/dev/null 2>&1 || (echo "Missing compiler: $(CXX)"; exit 1)
	@echo "[toolchain] $$($(CXX) --version | head -n 1)"

configure: $(BUILD_DIR)/.created
	@want_cxx="$$(command -v "$(CXX)" 2>/dev/null || echo "$(CXX)")"; \
	if [ -f "$(BUILD_DIR)/CMakeCache.txt" ]; then \
		cached_cxx="$$(grep -E '^CMAKE_CXX_COMPILER:FILEPATH=' "$(BUILD_DIR)/CMakeCache.txt" | cut -d= -f2)"; \
		if [ -n "$$cached_cxx" ] && [ "$$cached_cxx" != "$$want_cxx" ]; then \
			echo "[cmake] compiler changed ($$cached_cxx -> $$want_cxx), wiping $(BUILD_DIR)"; \
			rm -rf "$(BUILD_DIR)"; \
			mkdir -p "$(BUILD_DIR)"; \
			# recreate stamp after wipe\
			touch "$(BUILD_DIR)/.created"; \
		fi; \
	fi
	@echo "[cmake] configure ($(BUILD_TYPE)) - compiler: $(CXX)"
	cmake -S . -B "$(BUILD_DIR)" $(CMAKE_ARGS)

build: configure
	@echo "[cmake] build -j$(NPROC)"
	cmake --build "$(BUILD_DIR)" -j$(NPROC)

run: build
	./$(BUILD_DIR)/ffrog

install: build
	cmake --install "$(BUILD_DIR)"

clean:
	rm -rf "$(BUILD_DIR)"

distclean: clean
	rm -f compile_commands.json

reconfigure: clean build

help:
	@echo "Targets:"
	@echo "  make build              # configure + build (Release, clang++ + lld)"
	@echo "  make run                # build then run"
	@echo "  make clean              # remove build dir"
	@echo "  make reconfigure         # clean + build"
	@echo ""
	@echo "Knobs (examples):"
	@echo "  make BUILD_TYPE=RelWithDebInfo"
	@echo "  make USE_LLD=0"
	@echo "  make LTO=off"
	@echo "  make EXTRA_CXXFLAGS='-g'"
