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
MALFORMED_BIN := build/test_malformed
THREAD_BIN := build/test_threads
DESC_BIN := build/test_descriptor
STRUCT_BIN := build/test_struct
EMBEDDED_BIN := build/test_embedded
EMBEDDED_EXAMPLE_BIN := build/example_embedded
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
	-compatibility_version 1 -current_version 0.1.12
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

# Every binding ships one version: VERSION is the source of truth,
# scripts/sync-versions.sh propagates it (also a CI job).
version-check:
	./scripts/sync-versions.sh --check

$(LIB): $(OBJ)
	$(AR) rcs $@ $^

build/%.o: src/%.c $(HDRS) | build
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(TEST_BIN): tests/test_pbwire.c $(LIB) | build
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LIB)

$(MALFORMED_BIN): tests/test_malformed.c $(LIB) | build
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LIB)

build:
	mkdir -p build

$(THREAD_BIN): tests/test_threads.c $(TEST_HDRS) $(LIB) | build
	$(CC) $(CFLAGS) $(CPPFLAGS) -pthread -o $@ $< $(LIB)

test: $(TEST_BIN) $(MALFORMED_BIN) $(THREAD_BIN) $(DESC_BIN) $(STRUCT_BIN) $(EMBEDDED_BIN) $(EXAMPLE_BIN) $(EMBEDDED_EXAMPLE_BIN)
	./$(TEST_BIN)
	./$(MALFORMED_BIN)
	./$(THREAD_BIN)
	./$(DESC_BIN)
	./$(STRUCT_BIN)
	./$(EMBEDDED_BIN)
	./$(EXAMPLE_BIN)
	./$(EMBEDDED_EXAMPLE_BIN)

# Erlang binding (bindings/erlang): OTP app with its own rebar3 build.
# Needs Erlang/OTP (CI installs it); rebar3 is bootstrapped by its Makefile.
erlang-test:
	$(MAKE) -C bindings/erlang tests c-tests

# Erlang binding benchmark (bindings/erlang/benchmarks): epb, gpb and (when
# protoc + libprotobuf are available) gpb NIF on matrix3.proto. Same optional
# status as `bench`/`bench-c`; needs OTP + gpb.
erlang-bench:
	./scripts/bench-run.sh --binding erlang -- $(MAKE) -C bindings/erlang/benchmarks benchmarks

# C binding plugin (bindings/c/plugin): self-contained Go module, needs Go.
# Stays out of `make test` like the other optional targets.
plugin-c:
	$(MAKE) -C bindings/c/plugin test

# Go binding (bindings/go): cgo dynamic binding over the shared C core.
# Loads a FileDescriptorSet at run time, so schemas hot-swap without a
# restart. Needs Go and a C compiler only (same optional status as the
# other cgo/FFI bindings).
GO ?= go

go-test: $(LIB)
	cd bindings/go && $(GO) test ./...

go-bench: $(LIB)
	./scripts/bench-run.sh --binding go -- sh bindings/go/benchmarks/bench.sh

$(DESC_BIN): tests/test_descriptor.c $(TEST_HDRS) $(LIB) | build
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LIB)

C_RUNTIME := bindings/c/runtime/pbw_struct.c
$(STRUCT_BIN): bindings/c/tests/test_struct.c $(C_RUNTIME) $(LIB) | build
	$(CC) $(CFLAGS) $(CPPFLAGS) -Ibindings/c/runtime -Ibindings/c/tests -Ibindings/c/examples -o $@ bindings/c/tests/test_struct.c $(C_RUNTIME) $(LIB)

$(EMBEDDED_BIN): bindings/embedded/tests/test_embedded.c bindings/embedded/runtime/pbwe.c $(LIB) | build
	$(CC) $(CFLAGS) $(CPPFLAGS) -Iinclude -Ibindings/embedded/runtime -o $@ bindings/embedded/tests/test_embedded.c bindings/embedded/runtime/pbwe.c $(LIB)

$(EMBEDDED_EXAMPLE_BIN): bindings/embedded/examples/roundtrip.c bindings/embedded/runtime/pbwe.c $(LIB) | build
	$(CC) $(CFLAGS) $(CPPFLAGS) -Iinclude -Ibindings/embedded/runtime -o $@ bindings/embedded/examples/roundtrip.c bindings/embedded/runtime/pbwe.c $(LIB)
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

COMPAT_ORACLE_PB := bindings/cpp/tests/gen_oracle/matrix3.pb.cc
bindings/cpp/tests/gen_oracle/%.pb.cc: bindings/c/tests/data/%.proto
	@mkdir -p bindings/cpp/tests/gen_oracle
	$(PROTOC) -Ibindings/c/tests/data --cpp_out=bindings/cpp/tests/gen_oracle $<
COMPAT_ORACLE_BIN := build/compat_oracle
$(COMPAT_ORACLE_BIN): bindings/cpp/tests/compat_oracle.cc $(COMPAT_ORACLE_PB) $(CPP_RUNTIME_OBJ) $(LIB) | build
	$(BENCH_CXX) -std=c++17 -O2 -pthread -Iinclude -Ibindings/c/runtime \
		-Ibindings/c/tests -Ibindings/cpp/tests/gen_oracle $(PB_CXXFLAGS) \
		-o $@ bindings/cpp/tests/compat_oracle.cc $(COMPAT_ORACLE_PB) $(CPP_RUNTIME_OBJ) $(LIB) $(PB_LIBS)
compat-oracle: $(COMPAT_ORACLE_BIN)
	./$(COMPAT_ORACLE_BIN)

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
	# No cd: PROTOC may be relative to the repo root (CI exports an
	# absolute path, developers often rely on PATH), and protoc resolves
	# the inputs through -I either way.
	$(PROTOC) -I$(PROTOBUF_SRC)/src -I$(PROTOBUF_SRC) -I$(PROTOBUF_SRC)/editions/golden --include_imports \
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
		$(PY_ABS) bindings/python/conformance/testee.py \
		$(CURDIR)/$(CONFORMANCE_DESC)

# Same suite through the Lua binding (binary, JSON and text; only JSPB
# answers "skipped").
conformance-lua: $(CONFORMANCE_DESC) lua-bind
	@test -x "$(CONFORMANCE_RUNNER)" || { \
		echo "conformance_test_runner not found: $(CONFORMANCE_RUNNER)"; \
		echo "run 'make conformance-setup' first"; exit 1; }
	$(CONFORMANCE_RUNNER) --failure_list $(CURDIR)/conformance/failure_list_pbwire.txt \
		$(LUA_ABS) bindings/lua/conformance/testee.lua \
		$(CURDIR)/$(CONFORMANCE_DESC)

# Same suite through the R binding (binary, JSON and text; only JSPB
# answers "skipped").
conformance-r: $(CONFORMANCE_DESC) r-bind
	@test -x "$(CONFORMANCE_RUNNER)" || { \
		echo "conformance_test_runner not found: $(CONFORMANCE_RUNNER)"; \
		echo "run 'make conformance-setup' first"; exit 1; }
	$(CONFORMANCE_RUNNER) --failure_list $(CURDIR)/conformance/failure_list_pbwire.txt \
		$(RSCRIPT_ABS) bindings/r/conformance/testee.R \
		$(CURDIR)/$(CONFORMANCE_DESC)
conformance-ruby: $(CONFORMANCE_DESC) ruby-bind
	@test -x "$(CONFORMANCE_RUNNER)" || { \
		echo "conformance_test_runner not found: $(CONFORMANCE_RUNNER)"; \
		echo "run 'make conformance-setup' first"; exit 1; }
	$(CONFORMANCE_RUNNER) \
		$(RUBY_ABS) bindings/ruby/conformance/testee.rb \
		$(CURDIR)/$(CONFORMANCE_DESC)

# Same suite through the Java binding (binary wire format; JSON/text answer
# "skipped" because the binding has no printers). The runner execv()s the
# testee program directly, so the interpreter has to be an absolute path.
conformance-java: java-test $(CONFORMANCE_DESC)
	@test -x "$(CONFORMANCE_RUNNER)" || { \
		echo "conformance_test_runner not found: $(CONFORMANCE_RUNNER)"; \
		echo "run 'make conformance-setup' first"; exit 1; }
	$(CONFORMANCE_RUNNER) --failure_list $(CURDIR)/conformance/failure_list_pbwire.txt \
		$(JAVA_ABS) -cp $(CURDIR)/bindings/java/out Testee \
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
	bindings/erlang/c_src_tests/stub/*.c bindings/erlang/c_src_tests/stub/*.h \
	bindings/node/runtime/*.c bindings/objc/*.h bindings/objc/*.m \
	bindings/objc/examples/*.m bindings/objc/tests/*.m bindings/objc/benchmarks/*.m

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
	./scripts/bench-run.sh --binding core -- ./$(BENCH_BIN) bench

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
	./scripts/bench-run.sh --binding c -- ./$(BENCH_C_BIN) bench

# Python binding (bindings/python): a CPython extension that converts
# dict <-> wire in C. Needs the core library and a C compiler; no generated
# Python code and no setuptools.
PY ?= python3
# The conformance runner execv()s the testee program directly (no PATH search),
# so the interpreter has to be an absolute path.
PY_ABS := $(shell command -v $(PY) 2>/dev/null || echo $(PY))
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

# Host-language bindings over the same C ABI (see bindings/README.md). Each
# loads a FileDescriptorSet at run time, so none of them needs a plugin or a
# generator: Java (JDK 22+ Foreign Function & Memory API), Ruby (a C extension
# linked against the C core), Node (N-API addon, headers ship with node),
# Objective-C (ARC + Foundation). They need only their own toolchain and are not
# part of the commit gate.
JAVA ?= java
JAVA_ABS := $(shell command -v $(JAVA) 2>/dev/null || echo $(JAVA))
RUBY ?= ruby
RUBY_ABS := $(shell command -v $(RUBY) 2>/dev/null || echo $(RUBY))

java-test:
	cd bindings/java && ./build.sh

# Builds lib/pbwire_ext.<DLEXT> for $(RUBY); the core is linked in statically,
# so the binding needs no library path at run time.
ruby-bind:
	RUBY=$(RUBY) sh bindings/ruby/build.sh

ruby-test: ruby-bind
	$(RUBY) bindings/ruby/test/test_pbwire.rb

LUA ?= lua
LUA_ABS := $(shell command -v $(LUA) 2>/dev/null || echo $(LUA))

# Builds bindings/lua/pbwire.so for $(LUA); the core is linked in
# statically, so the binding needs no library path at run time.
lua-bind:
	LUA=$(LUA) sh bindings/lua/build.sh

lua-test: lua-bind
	cd bindings/lua && $(LUA) tests/test.lua
	cd bindings/lua && $(LUA) tests/test_fuzz.lua
	cd bindings/lua && $(LUA) examples/roundtrip.lua

node-test:
	./bindings/node/build.sh

NODE ?= node
NODE_ABS := $(shell command -v $(NODE) 2>/dev/null || echo $(NODE))

# Same suite through the Node binding (binary, JSON and text; only JSPB
# answers "skipped").
conformance-node: $(CONFORMANCE_DESC) node-test
	@test -x "$(CONFORMANCE_RUNNER)" || { \
		echo "conformance_test_runner not found: $(CONFORMANCE_RUNNER)"; \
		echo "run 'make conformance-setup' first"; exit 1; }
	$(CONFORMANCE_RUNNER) --failure_list $(CURDIR)/conformance/failure_list_pbwire.txt \
		$(NODE_ABS) bindings/node/conformance/testee.js \
		$(CURDIR)/$(CONFORMANCE_DESC)

objc-test:
	./bindings/objc/build.sh test

# The rest of the official languages, same rule: no plugin, no second wire
# implementation. Kotlin reuses the Java FFM layer (protoc --kotlin_out is also
# just Kotlin over protobuf-java), Rust links build/libpbwire.a in statically,
# and PHP, Dart and C# dlopen the shared library. Each needs only its own
# toolchain; none is part of the commit gate.
PHP ?= php
DART ?= dart
DOTNET ?= dotnet

kotlin-test: shared
	cd bindings/kotlin && ./build.sh
	cd bindings/kotlin && $(JAVA) --enable-native-access=ALL-UNNAMED \
		-Dpbwire.library=$(CURDIR)/$(SHLIB_LINK) \
		-cp out/pbwire.jar:../java/out TestPbwireKt ../c/tests/data/matrix3.desc

php-test: shared
	$(PHP) bindings/php/tests/test.php

# Native sibling of the FFI wrapper (core compiled in, like the Ruby C
# extension). Not part of php-test: it needs phpize, and the FFI version is
# the default. Both classes share names; the wrapper yields when loaded.
php-ext:
	cd bindings/php/ext && ./build.sh

php-ext-test: php-ext shared
	$(PHP) -d extension=$(CURDIR)/bindings/php/ext/modules/pbwire.so bindings/php/tests/test.php
	$(PHP) -d extension=$(CURDIR)/bindings/php/ext/modules/pbwire.so bindings/php/tests/generated_test.php

dart-test: shared
	cd bindings/dart && $(DART) pub get >/dev/null
	cd bindings/dart && $(DART) run tests/selftest.dart ../c/tests/data/matrix3.desc

rust-test: shared
	cd bindings/rust && cargo test

csharp-test: shared
	cd bindings/csharp && $(DOTNET) run --project tests -- ../c/tests/data/matrix3.desc

SWIFT ?= swift
SCALAC ?= scalac
ELIXIR ?= elixir
ZIG ?= zig
CLOJURE ?= clojure
JULIA ?= julia
RSCRIPT ?= Rscript
RSCRIPT_ABS := $(shell command -v $(RSCRIPT) 2>/dev/null || echo $(RSCRIPT))
r-bind:
	cd bindings/r && ./build.sh

swift-test: $(LIB)
	cd bindings/swift && ./build.sh test

scala-test: shared
	cd bindings/scala && ./build.sh

elixir-test:
	cd bindings/elixir && make test

zig-test:
	cd bindings/zig && $(ZIG) build test

fsharp-test: shared
	cd bindings/fsharp && $(DOTNET) run --project tests -- ../c/tests/data/matrix3.desc

clojure-test:
	cd bindings/clojure && ./build.sh

julia-test:
	cd bindings/julia && $(JULIA) --project=runtime tests/runtests.jl

r-test: r-bind
	cd bindings/r && $(RSCRIPT) tests/test_pbwire.R && $(RSCRIPT) tests/test.R

# Each binding benchmarks itself against its language's official runtime on
# matrix3.proto (same four tiers as `bench`/`bench-c`). Optional like those:
# they need protoc plus that runtime (a Maven jar, npm, the google-protobuf
# gem, the protobuf source for the ObjC runtime). RUBY picks the interpreter
# whose gem set has google-protobuf.
java-bench:
	./scripts/bench-run.sh --binding java -- ./bindings/java/benchmarks/bench.sh

ruby-bench: ruby-bind
	RUBY=$(RUBY) ./scripts/bench-run.sh --binding ruby -- sh bindings/ruby/benchmarks/bench.sh

lua-bench: lua-bind
	LUA=$(LUA) ./scripts/bench-run.sh --binding lua -- sh bindings/lua/benchmarks/bench.sh

node-bench:
	./scripts/bench-run.sh --binding node -- ./bindings/node/benchmarks/bench.sh

objc-bench:
	./scripts/bench-run.sh --binding objc -- ./bindings/objc/benchmarks/bench.sh

kotlin-bench:
	./scripts/bench-run.sh --binding kotlin -- ./bindings/kotlin/benchmarks/bench.sh

php-bench:
	./scripts/bench-run.sh --binding php -- ./bindings/php/benchmarks/bench.sh

dart-bench:
	./scripts/bench-run.sh --binding dart -- ./bindings/dart/benchmarks/bench.sh

rust-bench:
	./scripts/bench-run.sh --binding rust -- ./bindings/rust/benchmarks/bench.sh

csharp-bench:
	./scripts/bench-run.sh --binding csharp -- ./bindings/csharp/benchmarks/bench.sh

swift-bench:
	./scripts/bench-run.sh --binding swift -- ./bindings/swift/benchmarks/bench.sh

scala-bench:
	./scripts/bench-run.sh --binding scala -- ./bindings/scala/benchmarks/bench.sh

elixir-bench:
	./scripts/bench-run.sh --binding elixir -- make -C bindings/elixir bench

zig-bench:
	./scripts/bench-run.sh --binding zig -- ./bindings/zig/benchmarks/bench.sh --baseline

fsharp-bench:
	./scripts/bench-run.sh --binding fsharp -- ./bindings/fsharp/benchmarks/bench.sh

clojure-bench:
	./scripts/bench-run.sh --binding clojure -- ./bindings/clojure/benchmarks/bench.sh

julia-bench:
	./scripts/bench-run.sh --binding julia -- $(JULIA) --project=bindings/julia/runtime bindings/julia/benchmarks/bench.jl

r-bench:
	./scripts/bench-run.sh --binding r -- ./bindings/r/benchmarks/bench.sh

# The benchmark compares against the official Python runtime, so it needs
# protoc --python_out plus `pip install protobuf`.
bindings/python/benchmarks/gen/matrix3_pb2.py: bindings/c/tests/data/matrix3.proto
	@mkdir -p bindings/python/benchmarks/gen
	$(PROTOC) -Ibindings/c/tests/data --python_out=bindings/python/benchmarks/gen $<

python-bench: python-bind bindings/python/benchmarks/gen/matrix3_pb2.py
	./scripts/bench-run.sh --binding python -- $(PY) bindings/python/benchmarks/bench.py bench

difftest: $(BENCH_BIN)
	./$(BENCH_BIN) difftest

# Generated message models (codegen): one protoc plugin that emits generated
# message definitions per language, on top of each binding's dynamic API (no
# second wire implementation). `codegen` regenerates the committed models,
# `codegen-test` runs every binding's generated test, `codegen-conformance` generates
# models for the real conformance proto (needs `make conformance-setup`).
codegen:
	$(MAKE) -C codegen gen

codegen-test:
	$(MAKE) -C codegen test

codegen-conformance:
	$(MAKE) -C codegen conformance

clean:
	rm -rf build codegen/build bench/gen bindings/go/benchmarks/gen \
		bindings/java/benchmarks/gen bindings/java/benchmarks/out \
		bindings/node/benchmarks/gen bindings/ruby/benchmarks/gen \
		bindings/ruby/lib/pbwire_ext.* bindings/objc/benchmarks/gen \
		bindings/kotlin/out bindings/php/benchmarks/gen \
		bindings/dart/benchmarks/gen bindings/csharp/benchmarks/gen \
		bindings/csharp/*/obj bindings/csharp/*/bin \
		bindings/rust/target bindings/rust/benchmarks/gen \
	bindings/lua/pbwire.so bindings/lua/pbwire.o

.PHONY: all test examples shared format format-check bench bench-c erlang-test erlang-bench \
	python-bind python-test python-bench difftest conformance conformance-setup conformance-python \
	conformance-ruby conformance-java conformance-lua conformance-r conformance-node ruby-bind java-test ruby-test node-test objc-test r-bind \
	lua-bind lua-test lua-bench \
	java-bench ruby-bench node-bench objc-bench go-bench \
	kotlin-test php-test dart-test rust-test csharp-test \
	kotlin-test php-test dart-test rust-test csharp-test \
	kotlin-bench php-bench dart-bench rust-bench csharp-bench \
	swift-test scala-test elixir-test zig-test fsharp-test clojure-test julia-test r-test \
	swift-bench scala-bench elixir-bench zig-bench fsharp-bench clojure-bench julia-bench r-bench \
	desc-fixture test-cpp clean plugin-c go-test \
	codegen codegen-test codegen-conformance
