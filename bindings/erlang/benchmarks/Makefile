# Benchmarks for pbwire against gpb (same harness as every other
# binding: bindings/c/tests/data/matrix3.proto, four payload tiers).
#
# Prerequisites: Erlang/OTP, gpb (via the parent test profile or GPB_PATH).
# The gpb NIF baseline additionally needs protoc + libprotobuf C++; it is built
# and measured automatically when both are available.
#
# Quick start:
#   make build              # compile parent + the gpb record modules
#   make benchmarks         # run every backend that is available
#   make benchmarks-quick   # same, with a short sample time
#   make update-readme      # run benchmarks and refresh README.md files

ROOT       ?= ..
TOOL_SH    := $(abspath $(ROOT)/scripts/with_tool_versions.sh)
REBAR      ?= $(TOOL_SH) $(ROOT)/rebar3
GPB_PATH   ?= $(ROOT)/_build/test/lib/gpb
GPB_EBIN   ?= $(if $(wildcard $(GPB_PATH)/ebin/gpb_compile.beam),$(GPB_PATH)/ebin,$(abspath ../gpb/ebin))
GPB_INCLUDE ?= $(abspath $(GPB_EBIN)/../include)

EPB_EBIN ?= $(ROOT)/_build/default/lib/pbwire/ebin

# Erlang from repo .tool-versions (asdf/mise); override only if needed.
erl     ?= $(shell $(TOOL_SH) --erl)
erlc    ?= $(shell $(TOOL_SH) --erlc)
ERLANG_VERSION ?= $(shell $(TOOL_SH) --erlang-version)
ERL_ROOT ?= $(shell $(erl) -noshell -eval 'io:format("~s",[code:root_dir()]), halt().')
ERL_NIF_CFLAGS ?= -I$(ERL_ROOT)/usr/include

MATRIX3     := $(abspath ../../c/tests/data/matrix3.proto)
MATRIX3_DIR := $(dir $(MATRIX3))

PROTOC           ?= $(shell which protoc 2>/dev/null)
PROTOBUF_CFLAGS  ?= $(shell pkg-config --cflags protobuf 2>/dev/null)
PROTOBUF_LDFLAGS ?= $(shell pkg-config --libs protobuf 2>/dev/null)
CXX              ?= c++
CXXSTD           ?= -std=c++17
UNAME_S          ?= $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
NIF_LDFLAGS      ?= -undefined dynamic_lookup
else
NIF_LDFLAGS      ?=
endif

# The gpb NIF baseline needs protoc and the protobuf C++ runtime.
HAVE_NIF := $(if $(PROTOC),$(shell pkg-config --exists protobuf 2>/dev/null && echo yes))

.PHONY: all build benchmarks benchmarks-quick benchmarks-nif-code \
	update-readme show-hw-sw-info clean

all: build

build: check-erlang parent-compile benchmarks-code

check-erlang:
	@$(TOOL_SH) --check

parent-compile: check-erlang
	@test -x $(ROOT)/rebar3 || (echo "Missing $(ROOT)/rebar3; run 'make -C $(ROOT)' first." && exit 1)
	$(REBAR) compile
	@test -f $(GPB_EBIN)/gpb_compile.beam || $(REBAR) as test compile

benchmarks-code: tmp/matrix3_r.beam

benchmarks-nif-code: tmp/matrix3.beam tmp/matrix3.nif.so

benchmarks: build
ifneq ($(HAVE_NIF),)
	@$(MAKE) benchmarks-nif-code
endif
	$(PROTO_BENCH)

benchmarks-quick: build
ifneq ($(HAVE_NIF),)
	@$(MAKE) benchmarks-nif-code
endif
	BENCH_TARGET_TIME=0.05 $(PROTO_BENCH)

update-readme: build
	@./update_readme.sh

show-hw-sw-info:
	@$(call show-hw-sw-info)

PROTO_BENCH = $(TOOL_SH) ./proto-bench

define show-hw-sw-info
	echo "tool-versions erlang: $(ERLANG_VERSION)"; \
	echo "erl: $(erl)"; \
	echo "System"; \
	uname -srvm 2>/dev/null || uname -a; \
	if [ -r /proc/cpuinfo ]; then \
	  echo "CPU info"; \
	  egrep '^model name' /proc/cpuinfo 2>/dev/null | head -1; \
	  egrep '^cache size' /proc/cpuinfo 2>/dev/null | head -1; \
	  printf 'cores/threads   : %s\n' $$(egrep -c '^processor' /proc/cpuinfo 2>/dev/null); \
	  egrep '^bogomips' /proc/cpuinfo 2>/dev/null | head -1; \
	elif command -v sysctl >/dev/null 2>&1; then \
	  echo "CPU info"; \
	  sysctl -n machdep.cpu.brand_string 2>/dev/null; \
	  sysctl -n hw.physicalcpu 2>/dev/null | awk '{printf "physical cpus  : %s\n", $$1}'; \
	  sysctl -n hw.logicalcpu 2>/dev/null | awk '{printf "logical cpus   : %s\n", $$1}'; \
	fi; \
	echo; \
	$(erl) -noshell -eval 'io:format("OTP release ~s~n", [erlang:system_info(otp_release)]), io:format("~s~n", [erlang:system_info(system_version)]), halt().'
endef

# get_proto_defs/0 carries the proto3 markers and the defs-format-3 enum
# values that pbwire:load_cache/1 needs.
gpb-compile = \
	$(erl) -boot start_clean -pa $(GPB_EBIN) -noshell -noinput +B \
		-v never -c false -I$(MATRIX3_DIR) -o tmp \
		$(1) -introspect-get_proto_defs \
		-s gpb_compile c $(MATRIX3)

tmp/matrix3_r.beam: tmp/matrix3_r.erl
	$(erlc) -Wall -I$(GPB_INCLUDE) -o tmp $<

tmp/matrix3_r.erl: $(MATRIX3)
	@mkdir -p tmp
	$(call gpb-compile,-modsuffix _r)

tmp/matrix3.beam: tmp/matrix3.nif.cc
	$(erlc) -Wall -I$(GPB_INCLUDE) -o tmp tmp/matrix3.erl

# One gpb invocation writes matrix3.erl, matrix3.hrl and matrix3.nif.cc.
tmp/matrix3.nif.cc: $(MATRIX3)
	@mkdir -p tmp
	$(call gpb-compile,-nif)

tmp/matrix3.nif.so: tmp/matrix3.nif.cc
	$(PROTOC) -I$(MATRIX3_DIR) --cpp_out=tmp $(MATRIX3)
	$(CXX) -g -fPIC -O3 $(CXXSTD) $(PROTOBUF_CFLAGS) $(CXXFLAGS) \
		-o tmp/matrix3.pb.o -c tmp/matrix3.pb.cc
	$(CXX) -g -fPIC -O3 $(CXXSTD) $(ERL_NIF_CFLAGS) $(PROTOBUF_CFLAGS) $(CXXFLAGS) \
		-o tmp/matrix3.nif.o -c tmp/matrix3.nif.cc
	$(CXX) -g -fPIC -shared -O3 $(CXXSTD) $(NIF_LDFLAGS) \
		-o $@ tmp/matrix3.nif.o tmp/matrix3.pb.o $(PROTOBUF_LDFLAGS)

clean:
	$(RM) -r tmp
