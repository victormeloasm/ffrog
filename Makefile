# ffrog - Makefile wrapper around CMake (portable, fast, sane)
# - Prefers clang/clang++ if available, otherwise falls back to cc/c++ (often GCC).
# - Uses lld when available, BUT avoids GCC+LTO+lld (GCC LTO is not supported by lld).
# - Portable flags: NO -march=native / -mtune=native.

SHELL := /bin/bash

# ---------------- User knobs ----------------
BUILD_DIR   ?= build
CMAKE_GEN   ?= Unix Makefiles
BUILD_TYPE  ?= Release

# If clang exists, we default to it; otherwise fallback to system compiler.
_CLANG_CC  := $(shell command -v clang  2>/dev/null)
_CLANG_CXX := $(shell command -v clang++ 2>/dev/null)

CC  ?= $(if $(_CLANG_CC),clang,cc)
CXX ?= $(if $(_CLANG_CXX),clang++,c++)

# Linker: 1 = try lld, 0 = system default
USE_LLD ?= 1

# LTO modes: auto | off | thin | full
# auto: thin for clang, full for gcc.
LTO ?= auto

# Extra flags (optional)
EXTRA_CFLAGS    ?=
EXTRA_CXXFLAGS  ?=
EXTRA_LDFLAGS   ?=

# Binary name expected in build dir
RUN_BIN ?= ffrog

# ---------------- Internals ----------------
NPROC := $(shell nproc 2>/dev/null || echo 4)

IS_CLANG := $(shell $(CXX) --version 2>/dev/null | head -n1 | grep -qi clang && echo 1 || echo 0)
HAVE_LLD := $(shell command -v ld.lld >/dev/null 2>&1 && echo 1 || echo 0)

# Decide effective LTO
ifeq ($(LTO),auto)
  ifeq ($(IS_CLANG),1)
    LTO_EFFECTIVE := thin
  else
    LTO_EFFECTIVE := full
  endif
else
  LTO_EFFECTIVE := $(LTO)
endif

# If user asked for thin LTO but compiler isn't clang, downgrade to full.
ifeq ($(LTO_EFFECTIVE),thin)
  ifeq ($(IS_CLANG),0)
    LTO_EFFECTIVE := full
  endif
endif

# Decide effective lld usage (avoid GCC+LTO+lld)
USE_LLD_EFFECTIVE := $(USE_LLD)

ifeq ($(USE_LLD_EFFECTIVE),1)
  ifeq ($(HAVE_LLD),0)
    USE_LLD_EFFECTIVE := 0
  endif
endif

# GCC+LTO+lld is not supported (causes "undefined symbol: main", etc).
ifeq ($(IS_CLANG),0)
  ifeq ($(USE_LLD_EFFECTIVE),1)
    ifneq ($(LTO_EFFECTIVE),off)
      USE_LLD_EFFECTIVE := 0
      $(warning [cfg] GCC + LTO + lld is not supported. Disabling lld (using system linker).)
    endif
  endif
endif

# Common compile/link flags (portable)
CFLAGS_RELEASE   := -O3 -DNDEBUG -pipe -ffunction-sections -fdata-sections $(EXTRA_CFLAGS)
CXXFLAGS_RELEASE := -O3 -DNDEBUG -pipe -ffunction-sections -fdata-sections $(EXTRA_CXXFLAGS)
LDFLAGS_COMMON   := -Wl,--gc-sections -Wl,-O2 -Wl,--as-needed $(EXTRA_LDFLAGS)

# Apply LTO flags
ifeq ($(LTO_EFFECTIVE),thin)
  # clang only
  CFLAGS_RELEASE   += -flto=thin
  CXXFLAGS_RELEASE += -flto=thin
  # lld understands this; if another linker ignores it, harmless.
  LDFLAGS_COMMON   += -Wl,--thinlto-jobs=$(NPROC)
else ifeq ($(LTO_EFFECTIVE),full)
  # gcc or clang full LTO
  CFLAGS_RELEASE   += -flto
  CXXFLAGS_RELEASE += -flto
endif

# Apply lld if enabled
ifeq ($(USE_LLD_EFFECTIVE),1)
  LDFLAGS_COMMON += -fuse-ld=lld
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

# ---------------- Targets ----------------
.PHONY: all configure build run clean distclean reconfigure info help

all: build

$(BUILD_DIR)/.created:
	@mkdir -p "$(BUILD_DIR)"
	@touch "$@"

# Always configure to avoid stale cache / wrong compiler paths.
configure: $(BUILD_DIR)/.created
	@echo "[cfg] BUILD_TYPE=$(BUILD_TYPE)"
	@echo "[cfg] CC=$(CC)  CXX=$(CXX)  (clang? $(IS_CLANG))"
	@echo "[cfg] USE_LLD=$(USE_LLD) (ld.lld present: $(HAVE_LLD)) -> effective=$(USE_LLD_EFFECTIVE)"
	@echo "[cfg] LTO=$(LTO) -> effective=$(LTO_EFFECTIVE)"
	@echo "[cfg] If you switch compilers, run: make reconfigure"
	cmake -S . -B "$(BUILD_DIR)" $(CMAKE_ARGS)

build: configure
	@echo "[build] -j$(NPROC)"
	cmake --build "$(BUILD_DIR)" -j$(NPROC)

run: build
	./$(BUILD_DIR)/$(RUN_BIN)

clean:
	rm -rf "$(BUILD_DIR)"

distclean: clean
	rm -f compile_commands.json

reconfigure: clean build

info:
	@echo "CC=$(CC)"
	@echo "CXX=$(CXX)"
	@echo "IS_CLANG=$(IS_CLANG)"
	@echo "HAVE_LLD=$(HAVE_LLD)"
	@echo "USE_LLD=$(USE_LLD) -> effective=$(USE_LLD_EFFECTIVE)"
	@echo "LTO=$(LTO) -> effective=$(LTO_EFFECTIVE)"
	@echo "BUILD_TYPE=$(BUILD_TYPE)"
	@echo "BUILD_DIR=$(BUILD_DIR)"

help:
	@echo "Targets: build | run | clean | reconfigure | info"
	@echo "Knobs: BUILD_TYPE=RelWithDebInfo, USE_LLD=0, LTO=off/thin/full/auto, CC=clang CXX=clang++"
