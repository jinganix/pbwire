CC ?= cc
CFLAGS ?= -O2 -std=c99 -Wall -Wextra -Werror -pedantic -fPIC
CPPFLAGS += -Iinclude -Isrc
AR ?= ar

SRC := src/wire.c src/arena.c src/schema.c src/message.c src/encode.c src/decode.c \
	src/ctx.c src/descriptor_set.c
OBJ := $(SRC:src/%.c=build/%.o)
LIB := build/libpbwire.a
TEST_BIN := build/test_pbwire
THREAD_BIN := build/test_threads
DESC_BIN := build/test_descriptor
HDRS := include/pbwire.h src/internal.h

all: $(LIB)

$(LIB): $(OBJ)
	$(AR) rcs $@ $^

build/%.o: src/%.c $(HDRS) | build
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(TEST_BIN): tests/test_pbwire.c $(LIB) | build
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LIB)

build:
	mkdir -p build

$(THREAD_BIN): tests/test_threads.c $(LIB) | build
	$(CC) $(CFLAGS) $(CPPFLAGS) -pthread -o $@ $< $(LIB)

test: $(TEST_BIN) $(THREAD_BIN) $(DESC_BIN)
	./$(TEST_BIN)
	./$(THREAD_BIN)
	./$(DESC_BIN)

$(DESC_BIN): tests/test_descriptor.c $(LIB) | build
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LIB)

# Regenerates the pinned descriptor set embedded in tests/test_descriptor.c.
# Optional (needs protoc); paste the output into the array by hand.
desc-fixture:
	$(PROTOC) -Itests/data --include_imports \
		--descriptor_set_out=/tmp/pbw_desc.pb tests/data/desc_proto3.proto
	xxd -i /tmp/pbw_desc.pb

# Optional: official Protobuf conformance suite, binary wire format only.
# Needs a Protobuf source checkout (v36.2) with conformance_test_runner built:
#   git clone --depth 1 --branch v36.2 https://github.com/protocolbuffers/protobuf
#   cmake -S protobuf -B protobuf/build -Dprotobuf_BUILD_CONFORMANCE=ON
#   cmake --build protobuf/build --target conformance_test_runner
# Then: PBWIRE_PROTOBUF_SRC=$PWD/protobuf make conformance
# The testee answers binary requests and reports every JSON/text test as
# skipped, so 0 unexpected failures means the whole binary suite passes.
PROTOBUF_SRC ?= $(PBWIRE_PROTOBUF_SRC)
CONFORMANCE_RUNNER ?= $(PROTOBUF_SRC)/build/conformance_test_runner
CONFORMANCE_DESC := build/conformance.desc
CONFORMANCE_TESTEE := build/conformance_pbwire

$(CONFORMANCE_DESC): | build
	@test -n "$(PROTOBUF_SRC)" || { \
		echo "set PBWIRE_PROTOBUF_SRC to a Protobuf source checkout"; exit 1; }
	cd $(PROTOBUF_SRC) && $(PROTOC) -Isrc -I. -Ieditions/golden --include_imports \
		--descriptor_set_out=$(CURDIR)/$(CONFORMANCE_DESC) \
		google/protobuf/test_messages_proto3.proto \
		google/protobuf/test_messages_proto2.proto \
		conformance/test_protos/test_messages_edition2023.proto \
		editions/golden/test_messages_proto3_editions.proto \
		editions/golden/test_messages_proto2_editions.proto

$(CONFORMANCE_TESTEE): conformance/testee.c $(LIB) | build
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LIB)

conformance: $(CONFORMANCE_DESC) $(CONFORMANCE_TESTEE)
	@test -x "$(CONFORMANCE_RUNNER)" || { \
		echo "conformance_test_runner not found: $(CONFORMANCE_RUNNER)"; exit 1; }
	$(CONFORMANCE_RUNNER) $(CURDIR)/$(CONFORMANCE_TESTEE) $(CURDIR)/$(CONFORMANCE_DESC)

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

CFILES := include/*.h include/pbwire-protoc/*.h src/*.c src/*.h \
	src/protoc/*.cc tests/*.c tests/protoc/*.cc bench/bench.cc conformance/testee.c

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
	$(BENCH_CXX) -std=c++17 -O2 -Iinclude -Ibench/gen $(PB_CXXFLAGS) \
		-o $@ bench/bench.cc $(BENCH_PB) $(LIB) $(PB_LIBS)

bench: $(BENCH_BIN)
	./$(BENCH_BIN) bench

difftest: $(BENCH_BIN)
	./$(BENCH_BIN) difftest

clean:
	rm -rf build bench/gen

.PHONY: all test format format-check bench difftest conformance desc-fixture clean
