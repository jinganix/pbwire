# Elixir binding: Pbwire (Elixir structs over the :pbwire NIF).
#
# The NIF is the published :pbwire Erlang package (same C core as
# bindings/erlang); its c_src makefile expects the core static library and
# headers at ../../../build and ../../../include relative to its own c_src,
# i.e. at this directory. This file builds that core library from the repo
# sources (read-only) into ./build, links ./include at the repo headers, and
# then delegates to mix. Nothing is written outside this directory.
#
#   make          deps + compile (core lib, hex deps, mix compile)
#   make tests    ExUnit self-check (parity with elixir-protobuf)
#   make bench    four standard tiers vs Protobuf.encode/decode
#   make c-format mix format (mix format --check-formatted in CI terms)

PBW_ROOT := $(abspath $(CURDIR)/../..)
CORE_SRCS := $(wildcard $(PBW_ROOT)/src/*.c)
CORE_OBJS := $(patsubst $(PBW_ROOT)/src/%.c,build/obj/%.o,$(CORE_SRCS))
CORE_LIB := $(CURDIR)/build/libpbwire.a
CORE_INCLUDE := $(CURDIR)/include

CC ?= cc
CFLAGS ?= -O3 -std=c99 -fPIC -Wall

MIX := $(shell which mix 2>/dev/null || echo mix)

.PHONY: all compile setup test tests bench c-format c-format-check clean

all: compile

setup: $(CORE_LIB) $(CORE_INCLUDE)
	mix local.hex --force >/dev/null
	mix deps.get

compile: setup
	mix compile

tests: setup
	mix test

test: tests

bench: setup
	mix run benchmarks/bench.exs

c-format:
	mix format

c-format-check:
	mix format --check-formatted

clean:
	mix clean 2>/dev/null || true
	$(RM) -r build $(CORE_INCLUDE)

$(CORE_LIB): $(CORE_OBJS)
	ar rcs $@ $(CORE_OBJS)

build/obj/%.o: $(PBW_ROOT)/src/%.c $(PBW_ROOT)/include/pbwire.h | build/obj
	$(CC) $(CFLAGS) -I$(PBW_ROOT)/include -c $< -o $@

build/obj:
	mkdir -p build/obj

$(CORE_INCLUDE):
	ln -s ../../include $(CORE_INCLUDE)
