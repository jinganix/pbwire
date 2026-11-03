CC ?= cc
CFLAGS ?= -O2 -std=c99 -Wall -Wextra -Werror -pedantic -fPIC
CXX ?= c++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -Werror -pedantic
CPPFLAGS += -Iinclude -Isrc
AR ?= ar

SRC := src/wire.c src/arena.c src/schema.c src/message.c src/encode.c src/decode.c \
	src/ctx.c src/descriptor_set.c src/text.c src/json.c src/stream.c
OBJ := $(SRC:src/%.c=build/%.o)
LIB := build/libpbwire.a
TEST_BIN := build/test_pbwire
THREAD_BIN := build/test_threads
DESC_BIN := build/test_descriptor
STRUCT_BIN := build/test_struct
EXAMPLE_BIN := build/example_addressbook
CPP_EXAMPLE_BIN := build/example_addressbook_cpp
HDRS := include/pbwire.h src/internal.h
TEST_HDRS := tests/desc_set.h

# Shared library for the bindings (python/, go/): same ABI number as
# PBW_ABI_VERSION and the CMake SOVERSION.
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
SHLIB := build/libpbwire.1.dylib
SHLIB_LINK := build/libpbwire.dylib
SHLIB_FLAGS := -dynamiclib -install_name @rpath/libpbwire.1.dylib \
	-compatibility_version 1 -current_version 0.1.0
else
SHLIB := build/libpbwire.so.1
SHLIB_LINK := build/libpbwire.so
SHLIB_FLAGS := -shared -Wl,-soname,libpbwire.so.1
endif

shared: $(SHLIB)

$(SHLIB): $(OBJ) | build
	$(CC) $(SHLIB_FLAGS) -o $@ $^
	ln -sf $(notdir $@) $(SHLIB_LINK)

all: $(LIB)

$(LIB): $(OBJ)
	$(AR) rcs $@ $^

build/%.o: src/%.c $(HDRS) | build
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(TEST_BIN): tests/test_pbwire.c $(LIB) | build
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LIB)

build:
	mkdir -p build

$(THREAD_BIN): tests/test_threads.c $(TEST_HDRS) $(LIB) | build
	$(CC) $(CFLAGS) $(CPPFLAGS) -pthread -o $@ $< $(LIB)

test: $(TEST_BIN) $(THREAD_BIN) $(DESC_BIN) $(STRUCT_BIN) $(EXAMPLE_BIN)
	./$(TEST_BIN)
	./$(THREAD_BIN)
	./$(DESC_BIN)
	./$(STRUCT_BIN)
	./$(EXAMPLE_BIN)

# Erlang binding (bindings/erlang): OTP app with its own rebar3 build.
# Needs Erlang/OTP (CI installs it); rebar3 is bootstrapped by its Makefile.
erlang-test:
	$(MAKE) -C bindings/erlang tests c-tests

# Erlang binding benchmark (bindings/erlang/benchmarks): gpb, gpb NIF and
# enif_protobuf. Same optional status as `bench`/`bench-c`; needs OTP + gpb.
erlang-bench:
	$(MAKE) -C bindings/erlang/benchmarks benchmarks

# C binding plugin (bindings/c/plugin): self-contained Go module, needs Go.
# Stays out of `make test` like the other optional targets.
plugin-c:
	$(MAKE) -C bindings/c/plugin test

# Go binding (bindings/go): protoc-gen-pbwire-go emits plain Go structs with
# straight-line codecs, no cgo and no runtime dependency. Tests replay pinned
# protoc requests and pinned reference bytes, so they need Go only; the
# benchmark additionally needs protoc, protoc-gen-go and google.golang.org/
# protobuf (same optional status as bench/bench-c).
GO ?= go
GO_PLUGIN_BIN := bindings/go/plugin/build/protoc-gen-pbwire-go
GO_BENCH_GEN := bindings/go/benchmarks/gen

$(GO_PLUGIN_BIN): $(shell find bindings/go/plugin -type f -name '*.go') \
		bindings/go/plugin/go.mod bindings/go/plugin/go.sum
	$(MAKE) -C bindings/go/plugin build

plugin-go: $(GO_PLUGIN_BIN)

go-test: $(GO_PLUGIN_BIN)
	cd bindings/go && $(GO) test ./...
	$(MAKE) -C bindings/go/plugin test

$(GO_BENCH_GEN)/official/matrix3.pb.go: bindings/c/tests/data/matrix3.proto
	@mkdir -p $(dir $@)
	$(PROTOC) -Ibindings/c/tests/data --go_out=$(GO_BENCH_GEN)/official \
		--go_opt=paths=source_relative \
		--go_opt=Mmatrix3.proto=pbwire/benchmarks/gen/official $<

$(GO_BENCH_GEN)/pbwire/matrix3.pbwire.go: bindings/c/tests/data/matrix3.proto $(GO_PLUGIN_BIN)
	@mkdir -p $(dir $@)
	$(PROTOC) -Ibindings/c/tests/data \
		--plugin=protoc-gen-pbwire-go=$(GO_PLUGIN_BIN) \
		--pbwire-go_out=$(GO_BENCH_GEN)/pbwire $<

go-bench: $(GO_BENCH_GEN)/official/matrix3.pb.go $(GO_BENCH_GEN)/pbwire/matrix3.pbwire.go
	cd bindings/go/benchmarks && $(GO) run .

$(DESC_BIN): tests/test_descriptor.c $(TEST_HDRS) $(LIB) | build
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LIB)

C_RUNTIME := bindings/c/runtime/pbw_struct.c
$(STRUCT_BIN): bindings/c/tests/test_struct.c $(C_RUNTIME) $(LIB) | build
	$(CC) $(CFLAGS) $(CPPFLAGS) -Ibindings/c/runtime -Ibindings/c/tests -Ibindings/c/examples -o $@ bindings/c/tests/test_struct.c $(C_RUNTIME) $(LIB)
$(EXAMPLE_BIN): bindings/c/examples/addressbook.c $(C_RUNTIME) $(LIB) | build
	$(CC) $(CFLAGS) $(CPPFLAGS) -Ibindings/c/runtime -o $@ bindings/c/examples/addressbook.c $(C_RUNTIME) $(LIB)

# C++ binding example (bindings/cpp): same addressbook round-trip as the
# C example, reusing the C runtime (compiled as C, linked into the C++
# binary). Stays out of `make test` (that gate is C99-only).
CPP_RUNTIME_OBJ := build/pbw_struct_c.o
$(CPP_RUNTIME_OBJ): $(C_RUNTIME) | build
	$(CC) $(CFLAGS) $(CPPFLAGS) -Ibindings/c/runtime -c -o $@ $(C_RUNTIME)
CPP_TEST_BIN := build/test_cpp_addressbook

$(CPP_TEST_BIN): bindings/cpp/tests/roundtrip.cc $(CPP_RUNTIME_OBJ) $(LIB) | build
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -Ibindings/c/runtime -Ibindings/c/examples -o $@ $< $(CPP_RUNTIME_OBJ) $(LIB)

test-cpp: $(CPP_TEST_BIN)
	./$(CPP_TEST_BIN)

$(CPP_EXAMPLE_BIN): bindings/cpp/examples/addressbook.cc $(CPP_RUNTIME_OBJ) $(LIB) | build
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -Ibindings/c/runtime -Ibindings/c/examples -o $@ bindings/cpp/examples/addressbook.cc $(CPP_RUNTIME_OBJ) $(LIB)

examples: $(EXAMPLE_BIN) $(CPP_EXAMPLE_BIN)
	./$(EXAMPLE_BIN)
	./$(CPP_EXAMPLE_BIN)

# Regenerates the pinned descriptor set embedded in tests/test_descriptor.c.
# Optional (needs protoc); paste the output into the array by hand.
desc-fixture:
	$(PROTOC) -Itests/data --include_imports \
		--descriptor_set_out=/tmp/pbw_desc.pb tests/data/desc_proto3.proto
	xxd -i /tmp/pbw_desc.pb

# Optional: official Protobuf conformance suite, binary wire format and text
# format. The reference checkout lives in .tmp/ (git-ignored, not a build
# artifact), so one clone serves every branch and is never re-pulled:
# `make conformance-setup` clones it once (shallow, pinned version) and builds
# the runner; later runs only rebuild incrementally. Override the location
# with PBWIRE_PROTOBUF_SRC=/path/to/protobuf.
# The testee answers binary and text requests and reports every JSON test as
# skipped. Two binary tests fail and are expected to: the reference C++ testee
# from the same checkout fails them identically (a 5-byte tag varint above
# UINT32_MAX is truncated instead of rejected). They are listed so the runner
# proceeds to the text suite instead of stopping after binary.
PROTOBUF_VERSION := v36.2
PROTOBUF_SRC = $(or $(PBWIRE_PROTOBUF_SRC),$(CURDIR)/.tmp/protobuf-$(PROTOBUF_VERSION))
CONFORMANCE_RUNNER ?= $(PROTOBUF_SRC)/build/conformance_test_runner
CONFORMANCE_DESC := build/conformance.desc
CONFORMANCE_TESTEE := build/conformance_pbwire

$(CONFORMANCE_DESC): | build
	@test -d "$(PROTOBUF_SRC)/src" || { \
		echo "protobuf checkout not found: $(PROTOBUF_SRC)"; \
		echo "run 'make conformance-setup' first (clones once, then reuses it)"; exit 1; }
	cd $(PROTOBUF_SRC) && $(PROTOC) -Isrc -I. -Ieditions/golden --include_imports \
		--descriptor_set_out=$(CURDIR)/$(CONFORMANCE_DESC) \
		google/protobuf/test_messages_proto3.proto \
		google/protobuf/test_messages_proto2.proto \
		conformance/conformance.proto \
		conformance/test_protos/test_messages_edition2023.proto \
		editions/golden/test_messages_proto3_editions.proto \
		editions/golden/test_messages_proto2_editions.proto

$(CONFORMANCE_TESTEE): conformance/testee.c $(LIB) | build
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LIB)

# Same suite through the Python binding (needs make python-bind first).
conformance-python: python-bind $(CONFORMANCE_DESC)
	@test -x "$(CONFORMANCE_RUNNER)" || { \
		echo "conformance_test_runner not found: $(CONFORMANCE_RUNNER)"; \
		echo "run 'make conformance-setup' first"; exit 1; }
	$(CONFORMANCE_RUNNER) \
		$(CURDIR)/$(PY) bindings/python/conformance/testee.py \
		$(CURDIR)/$(CONFORMANCE_DESC)

conformance: $(CONFORMANCE_DESC) $(CONFORMANCE_TESTEE)
	@test -x "$(CONFORMANCE_RUNNER)" || { \
		echo "conformance_test_runner not found: $(CONFORMANCE_RUNNER)"; \
		echo "run 'make conformance-setup' first (clones once, then reuses it)"; exit 1; }
	$(CONFORMANCE_RUNNER) --failure_list $(CURDIR)/conformance/failure_list_pbwire.txt \
		$(CURDIR)/$(CONFORMANCE_TESTEE) $(CURDIR)/$(CONFORMANCE_DESC)

# One-time (idempotent) setup for `make conformance`: shallow-clone the pinned
# protobuf checkout into the cache and build the runner. Re-runs skip the
# clone and only rebuild what changed.
conformance-setup:
	@if [ ! -d "$(PROTOBUF_SRC)/src/google/protobuf" ]; then \
		echo "cloning protobuf $(PROTOBUF_VERSION) (shallow) into $(PROTOBUF_SRC)"; \
		git clone --depth 1 --branch $(PROTOBUF_VERSION) \
			https://github.com/protocolbuffers/protobuf $(PROTOBUF_SRC); \
	else \
		echo "protobuf checkout exists: $(PROTOBUF_SRC)"; \
	fi
	cmake -S $(PROTOBUF_SRC) -B $(PROTOBUF_SRC)/build -Dprotobuf_BUILD_CONFORMANCE=ON \
		-DBUILD_SHARED_LIBS=ON
	cmake --build $(PROTOBUF_SRC)/build --target conformance_test_runner

# Optional: differential test and benchmark against the official Protobuf C++
# runtime. Needs protoc and libprotobuf (Homebrew: brew install protobuf), so it
# is deliberately outside `make test` and the commit gate.
PROTOC ?= protoc
PB_CXXFLAGS := $(shell pkg-config --cflags protobuf 2>/dev/null)
PB_LIBS := $(shell pkg-config --libs protobuf 2>/dev/null)
BENCH_CXX ?= c++
BENCH_BIN := build/bench
BENCH_PB := bench/gen/msg.pb.cc bench/gen/msg2.pb.cc
BENCH_DESC := bench/gen/msg.pb
BENCH_C_PB := bindings/c/benchmarks/gen/matrix3.pb.cc
BENCH_C_BIN := build/bench_struct_c

CFILES := include/*.h include/pbwire-protoc/*.h src/*.c src/*.h \
	src/protoc/*.cc tests/*.c tests/protoc/*.cc bench/bench.cc conformance/testee.c \
	bindings/c/benchmarks/*.cc bindings/python/runtime/*.c \
	bindings/c/runtime/*.c bindings/c/runtime/*.h \
	bindings/c/examples/*.c bindings/c/tests/*.c bindings/cpp/examples/*.cc \
	bindings/erlang/c_src/*.c bindings/erlang/c_src/*.h \
	bindings/erlang/c_src_tests/*.c bindings/erlang/c_src_tests/*.h \
	bindings/erlang/c_src_tests/stub/*.c bindings/erlang/c_src_tests/stub/*.h

format:
	clang-format -i $(CFILES)

format-check:
	clang-format --dry-run --Werror $(CFILES)

bench/gen/%.pb.cc: bench/%.proto
	@mkdir -p bench/gen
	$(PROTOC) -Ibench --cpp_out=bench/gen $<

bench/gen/%.pb: bench/%.proto
	@mkdir -p bench/gen
	$(PROTOC) -Ibench --include_imports --descriptor_set_out=$@ $<

$(BENCH_BIN): bench/bench.cc $(BENCH_PB) $(BENCH_DESC) $(LIB) | build
	$(BENCH_CXX) -std=c++17 -O2 -pthread -Iinclude -Ibench/gen $(PB_CXXFLAGS) \
		-o $@ bench/bench.cc $(BENCH_PB) $(LIB) $(PB_LIBS)

bench: $(BENCH_BIN)
	./$(BENCH_BIN) bench

# C binding benchmark (bindings/c/benchmarks): generated structs vs the
# official C++ generated code on matrix3.proto. Same optional deps as
# bench/difftest (protoc + libprotobuf), same spot outside `make test`.
bindings/c/benchmarks/gen/%.pb.cc: bindings/c/tests/data/%.proto
	@mkdir -p bindings/c/benchmarks/gen
	$(PROTOC) -Ibindings/c/tests/data --cpp_out=bindings/c/benchmarks/gen $<

$(BENCH_C_BIN): bindings/c/benchmarks/bench_struct.cc $(BENCH_C_PB) $(CPP_RUNTIME_OBJ) $(LIB) | build
	$(BENCH_CXX) -std=c++17 -O2 -pthread -Iinclude -Ibindings/c/runtime \
		-Ibindings/c/tests -Ibindings/c/benchmarks/gen $(PB_CXXFLAGS) \
		-o $@ bindings/c/benchmarks/bench_struct.cc $(BENCH_C_PB) $(CPP_RUNTIME_OBJ) $(LIB) $(PB_LIBS)

bench-c: $(BENCH_C_BIN)
	./$(BENCH_C_BIN) bench

# Python binding (bindings/python): a CPython extension that converts
# dict <-> wire in C. Needs the core library and a C compiler; no generated
# Python code and no setuptools.
PY ?= python3
PY_INC := $(shell $(PY) -c "import sysconfig; print('-I' + sysconfig.get_paths()['include'])")
PY_EXT := $(shell $(PY) -c "import sysconfig; print(sysconfig.get_config_var('EXT_SUFFIX'))")
PY_MODULE := bindings/python/pbwire$(PY_EXT)
ifeq ($(UNAME_S),Darwin)
PY_LDFLAGS := -undefined dynamic_lookup
else
PY_LDFLAGS :=
endif

$(PY_MODULE): bindings/python/runtime/pbwire.c $(LIB) | build
	$(CC) $(CFLAGS) $(CPPFLAGS) -fPIC -shared $(PY_INC) -Iinclude \
		$(PY_LDFLAGS) -o $@ $< $(LIB)

python-bind: $(PY_MODULE)

python-test: python-bind
	$(PY) bindings/python/tests/test_pbwire.py

# The benchmark compares against the official Python runtime, so it needs
# protoc --python_out plus `pip install protobuf`.
bindings/python/benchmarks/gen/matrix3_pb2.py: bindings/c/tests/data/matrix3.proto
	@mkdir -p bindings/python/benchmarks/gen
	$(PROTOC) -Ibindings/c/tests/data --python_out=bindings/python/benchmarks/gen $<

python-bench: python-bind bindings/python/benchmarks/gen/matrix3_pb2.py
	$(PY) bindings/python/benchmarks/bench.py bench

difftest: $(BENCH_BIN)
	./$(BENCH_BIN) difftest

clean:
	rm -rf build bench/gen bindings/go/benchmarks/gen

.PHONY: all test examples format format-check bench bench-c erlang-bench \
	python-bind python-test python-bench difftest conformance conformance-python \
	desc-fixture test-cpp clean plugin-c plugin-go go-test go-bench
