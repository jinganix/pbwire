# Based on c_src.mk from erlang.mk by Loic Hoguin <essen@ninenines.eu>

CURDIR := $(shell pwd)
BASEDIR := $(abspath $(CURDIR)/..)

PROJECT ?= pbwire
PROJECT := $(strip $(PROJECT))

ERTS_INCLUDE_DIR ?= $(shell erl -noshell -eval "io:format(\"~ts/erts-~ts/include/\", [code:root_dir(), erlang:system_info(version)])." -s erlang halt)
ERL_INTERFACE_INCLUDE_DIR ?= $(shell erl -noshell -eval "io:format(\"~ts\", [code:lib_dir(erl_interface, include)])." -s erlang halt)
ERL_INTERFACE_LIB_DIR ?= $(shell erl -noshell -eval "io:format(\"~ts\", [code:lib_dir(erl_interface, lib)])." -s erlang halt)

C_SRC_DIR = $(CURDIR)
BUILD = $(CURDIR)/build
C_SRC_OUTPUT ?= $(CURDIR)/../priv/$(PROJECT).so

# pbwire core, vendored under c_src/core so the Hex package compiles
# standalone: a Hex dependency has no repo root to link against.
# c_src/core/ is a byte-identical copy of <repo>/include + <repo>/src
# (*.c/*.h only, no protoc/); core-sync-check below enforces it whenever
# the repo root is present (a no-op for Hex checkouts).
PBW_CORE := $(C_SRC_DIR)/core
REPO_ROOT := $(abspath $(CURDIR)/../../../)
CORE_FILES := include/pbwire.h $(patsubst $(REPO_ROOT)/%,%,$(wildcard $(REPO_ROOT)/src/*.c $(REPO_ROOT)/src/*.h))

# All C sources under c_src/, including the vendored core: one NIF image.
SOURCES := $(shell find $(C_SRC_DIR) -type f \( -name "*.c" -o -name "*.C" -o -name "*.cc" -o -name "*.cpp" \) ! -path "*/test/*")

# System type and C compiler/flags.

ifeq ($(OS),Windows_NT)
	PRIV_DIR := $(BASEDIR)/priv
	# OTP on Windows is a native PE binary; link the NIF with MinGW-w64, not MSYS libc.
	ifneq ($(wildcard /mingw64/bin/gcc.exe),)
		CC := /mingw64/bin/gcc
	else ifneq ($(wildcard /ucrt64/bin/gcc.exe),)
		CC := /ucrt64/bin/gcc
	else
		CC ?= gcc
	endif
	MINGW_BIN := $(dir $(shell command -v $(CC) 2>/dev/null || echo $(CC)))
	CFLAGS ?= -O3 -std=c99 -finline-functions -Wall -Wmissing-prototypes
	CXXFLAGS ?= -O3 -finline-functions -Wall
	C_SRC_OUTPUT = $(CURDIR)/../priv/$(PROJECT).dll
	CFLAGS += -I "$(ERTS_INCLUDE_DIR)" -I "$(PBW_CORE)/include" -I "$(PBW_CORE)/src"
	CXXFLAGS += -I "$(ERTS_INCLUDE_DIR)" -I "$(PBW_CORE)/include" -I "$(PBW_CORE)/src"
	LDFLAGS += -shared -static-libgcc
	WIN_RUNTIME_DLLS := libgcc_s_seh-1.dll libwinpthread-1.dll
else
UNAME_SYS := $(shell uname -s)
ifeq ($(UNAME_SYS), Darwin)
    ARCH_SYS := $(shell uname -p)
    ifeq ($(ARCH_SYS), arm)
        ARCH_NAME := arm64
    else
        ARCH_NAME := x86_64
    endif
	CC ?= cc
	CFLAGS ?= -O3 -std=c99 -arch $(ARCH_NAME) -finline-functions -Wall -Wmissing-prototypes
	CXXFLAGS ?= -O3 -arch $(ARCH_NAME) -finline-functions -Wall
	LDFLAGS ?= -arch $(ARCH_NAME) -flat_namespace -undefined dynamic_lookup
else ifeq ($(UNAME_SYS), FreeBSD)
	CC ?= cc
	CFLAGS ?= -O3 -std=c99 -finline-functions -Wall -Wmissing-prototypes
	CXXFLAGS ?= -O3 -finline-functions -Wall
else ifeq ($(UNAME_SYS), Linux)
	CC ?= gcc
	CFLAGS ?= -O3 -std=c99 -finline-functions -Wall -Wmissing-prototypes
	CXXFLAGS ?= -O3 -finline-functions -Wall
endif

CFLAGS += -fPIC -I $(ERTS_INCLUDE_DIR) -I $(ERL_INTERFACE_INCLUDE_DIR) -I $(PBW_CORE)/include -I $(PBW_CORE)/src
CXXFLAGS += -fPIC -I $(ERTS_INCLUDE_DIR) -I $(ERL_INTERFACE_INCLUDE_DIR) -I $(PBW_CORE)/include -I $(PBW_CORE)/src

LDLIBS += -L $(ERL_INTERFACE_LIB_DIR) -lei -lpthread
LDFLAGS += -shared
endif

# Verbosity.

c_verbose_0 = @echo " C     " $(?F);
c_verbose = $(c_verbose_$(V))

cpp_verbose_0 = @echo " CPP   " $(?F);
cpp_verbose = $(cpp_verbose_$(V))

link_verbose_0 = @echo " LD    " $(@F);
link_verbose = $(link_verbose_$(V))

OBJECTS = $(addprefix $(BUILD)/,$(addsuffix .o,$(notdir $(basename $(SOURCES)))))

COMPILE_C = $(c_verbose) $(CC) $(CFLAGS) $(CPPFLAGS) -c
COMPILE_CPP = $(cpp_verbose) $(CXX) $(CXXFLAGS) $(CPPFLAGS) -c

$(C_SRC_OUTPUT): $(OBJECTS) | $(BUILD) core-sync-check
	@mkdir -p $(BASEDIR)/priv/
	$(link_verbose) $(CC) $(OBJECTS) $(LDFLAGS) $(LDLIBS) -o $(C_SRC_OUTPUT)
ifeq ($(OS),Windows_NT)
	@for dll in $(WIN_RUNTIME_DLLS); do \
		if [ -f "$(MINGW_BIN)$$dll" ]; then \
			cp -f "$(MINGW_BIN)$$dll" "$(PRIV_DIR)/"; \
		fi; \
	done
endif

# Rebuild objects when headers change; struct layouts live there.
$(OBJECTS): $(wildcard $(C_SRC_DIR)/*.h $(C_SRC_DIR)/core/include/*.h $(C_SRC_DIR)/core/src/*.h)

$(BUILD)/%.o: $(C_SRC_DIR)/%.c | $(BUILD)
	$(COMPILE_C) -o $@ $<

$(BUILD)/%.o: $(C_SRC_DIR)/core/src/%.c | $(BUILD)
	$(COMPILE_C) -o $@ $<

$(BUILD)/%.o: $(C_SRC_DIR)/%.cc | $(BUILD)
	$(COMPILE_CPP) -o $@ $<

$(BUILD)/%.o: $(C_SRC_DIR)/%.C | $(BUILD)
	$(COMPILE_CPP) -o $@ $<

$(BUILD)/%.o: $(C_SRC_DIR)/%.cpp | $(BUILD)
	$(COMPILE_CPP) -o $@ $<

$(BUILD):
	@mkdir -p $(BUILD)

# The vendored core must match the repo root; Hex checkouts have no root
# to compare against, so the check is a no-op there.
.PHONY: core-sync-check
core-sync-check:
	@if [ -d $(REPO_ROOT)/src ]; then \
		for f in $(CORE_FILES); do \
			cmp -s $(REPO_ROOT)/$$f $(PBW_CORE)/$$f || { echo "c_src/core drift: $$f differs from repo root; copy it over"; exit 1; }; \
		done; \
	fi

clean:
	@rm -rf $(BUILD)
	@rm -f $(C_SRC_DIR)/*.o $(C_SRC_DIR)/*.d
	@rm -f $(C_SRC_OUTPUT)
ifeq ($(OS),Windows_NT)
	@rm -f $(BASEDIR)/priv/libgcc_s_seh-1.dll $(BASEDIR)/priv/libwinpthread-1.dll
endif
