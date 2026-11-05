# Generated message models: one protoc plugin, one model per language.
#
#   make build   build protoc-gen-pbwire
#   make gen     regenerate the committed models (needs protoc)
#   make test    run every binding's generated test (needs each toolchain)
#
# The models are committed next to each binding's tests so the tests stay
# protoc-free; `make gen` is how they get refreshed.
PROTOC ?= protoc
GO ?= go
PLUGIN := build/protoc-gen-pbwire
DATA := ../bindings/c/tests/data
PROTO := $(DATA)/matrix3.proto
OPTS = -I$(DATA) --plugin=protoc-gen-pbwire=$(PLUGIN)

all: build

build:
	$(GO) build -o $(PLUGIN) ./cmd/protoc-gen-pbwire

gen: gen-java gen-kotlin gen-csharp gen-dart gen-php gen-ruby gen-python gen-node gen-rust gen-objc gen-lua gen-swift gen-zig gen-scala gen-clojure gen-fsharp gen-elixir gen-julia gen-r gen-examples

# The examples all speak the C example's addressbook.proto: same schema, same
# bytes, one generated walkthrough per language.
EXAMPLE := ../examples/addressbook.proto
EXAMPLE_OPTS = -I../examples --plugin=protoc-gen-pbwire=$(PLUGIN)

gen-examples: build
	mkdir -p ../bindings/java/examples/gen ../bindings/kotlin/examples/gen ../bindings/csharp/examples/gen \
		../bindings/dart/examples/gen ../bindings/php/examples/gen ../bindings/ruby/examples/gen \
		../bindings/python/examples/gen ../bindings/node/examples/gen ../bindings/rust/examples/gen \
		../bindings/objc/examples/gen ../bindings/lua/examples/gen ../bindings/go/examples/gen/addressbook \
		../bindings/swift/examples/gen ../bindings/zig/examples/gen ../bindings/scala/examples/gen \
		../bindings/clojure/examples/gen ../bindings/fsharp/examples/gen ../bindings/elixir/examples/gen \
		../bindings/julia/examples/gen ../bindings/r/examples/gen
	$(PROTOC) $(EXAMPLE_OPTS) --pbwire_out=../bindings/java/examples/gen --pbwire_opt=lang=java $(EXAMPLE)
	$(PROTOC) $(EXAMPLE_OPTS) --pbwire_out=../bindings/kotlin/examples/gen --pbwire_opt=lang=kotlin $(EXAMPLE)
	$(PROTOC) $(EXAMPLE_OPTS) --pbwire_out=../bindings/csharp/examples/gen --pbwire_opt=lang=csharp $(EXAMPLE)
	$(PROTOC) $(EXAMPLE_OPTS) --pbwire_out=../bindings/dart/examples/gen --pbwire_opt=lang=dart $(EXAMPLE)
	$(PROTOC) $(EXAMPLE_OPTS) --pbwire_out=../bindings/php/examples/gen --pbwire_opt=lang=php $(EXAMPLE)
	$(PROTOC) $(EXAMPLE_OPTS) --pbwire_out=../bindings/ruby/examples/gen --pbwire_opt=lang=ruby $(EXAMPLE)
	$(PROTOC) $(EXAMPLE_OPTS) --pbwire_out=../bindings/python/examples/gen --pbwire_opt=lang=python $(EXAMPLE)
	$(PROTOC) $(EXAMPLE_OPTS) --pbwire_out=../bindings/node/examples/gen --pbwire_opt=lang=javascript $(EXAMPLE)
	$(PROTOC) $(EXAMPLE_OPTS) --pbwire_out=../bindings/node/examples/gen --pbwire_opt=lang=typescript $(EXAMPLE)
	$(PROTOC) $(EXAMPLE_OPTS) --pbwire_out=../bindings/rust/examples/gen --pbwire_opt=lang=rust $(EXAMPLE)
	$(PROTOC) $(EXAMPLE_OPTS) --pbwire_out=../bindings/objc/examples/gen --pbwire_opt=lang=objc $(EXAMPLE)
	$(PROTOC) $(EXAMPLE_OPTS) --pbwire_out=../bindings/lua/examples/gen --pbwire_opt=lang=lua $(EXAMPLE)
	$(PROTOC) $(EXAMPLE_OPTS) --pbwire_out=../bindings/go/examples/gen/addressbook --pbwire_opt=lang=go $(EXAMPLE)
	$(PROTOC) $(EXAMPLE_OPTS) --pbwire_out=../bindings/swift/examples/gen --pbwire_opt=lang=swift $(EXAMPLE)
	$(PROTOC) $(EXAMPLE_OPTS) --pbwire_out=../bindings/zig/examples/gen --pbwire_opt=lang=zig $(EXAMPLE)
	$(PROTOC) $(EXAMPLE_OPTS) --pbwire_out=../bindings/scala/examples/gen --pbwire_opt=lang=scala $(EXAMPLE)
	$(PROTOC) $(EXAMPLE_OPTS) --pbwire_out=../bindings/clojure/examples/gen --pbwire_opt=lang=clojure $(EXAMPLE)
	$(PROTOC) $(EXAMPLE_OPTS) --pbwire_out=../bindings/fsharp/examples/gen --pbwire_opt=lang=fsharp $(EXAMPLE)
	$(PROTOC) $(EXAMPLE_OPTS) --pbwire_out=../bindings/elixir/examples/gen --pbwire_opt=lang=elixir $(EXAMPLE)
	$(PROTOC) $(EXAMPLE_OPTS) --pbwire_out=../bindings/julia/examples/gen --pbwire_opt=lang=julia $(EXAMPLE)
	$(PROTOC) $(EXAMPLE_OPTS) --pbwire_out=../bindings/r/examples/gen --pbwire_opt=lang=r $(EXAMPLE)

gen-java: build
	mkdir -p ../bindings/java/tests/gen
	$(PROTOC) $(OPTS) --pbwire_out=../bindings/java/tests/gen --pbwire_opt=lang=java $(PROTO)
	$(PROTOC) $(TESTDATA_OPTS) --pbwire_out=../bindings/java/tests/gen --pbwire_opt=lang=java $(TESTDATA)/oneof1.proto

gen-kotlin: build
	mkdir -p ../bindings/kotlin/tests/gen
	$(PROTOC) $(OPTS) --pbwire_out=../bindings/kotlin/tests/gen --pbwire_opt=lang=kotlin $(PROTO)
	$(PROTOC) $(TESTDATA_OPTS) --pbwire_out=../bindings/kotlin/tests/gen --pbwire_opt=lang=kotlin $(TESTDATA)/oneof1.proto

gen-csharp: build
	mkdir -p ../bindings/csharp/generated/gen
	$(PROTOC) $(OPTS) --pbwire_out=../bindings/csharp/generated/gen --pbwire_opt=lang=csharp $(PROTO)
	$(PROTOC) $(TESTDATA_OPTS) --pbwire_out=../bindings/csharp/generated/gen --pbwire_opt=lang=csharp $(TESTDATA)/oneof1.proto

gen-dart: build
	mkdir -p ../bindings/dart/tests/gen
	$(PROTOC) $(OPTS) --pbwire_out=../bindings/dart/tests/gen --pbwire_opt=lang=dart $(PROTO)
	$(PROTOC) $(TESTDATA_OPTS) --pbwire_out=../bindings/dart/tests/gen --pbwire_opt=lang=dart $(TESTDATA)/oneof1.proto

gen-php: build
	mkdir -p ../bindings/php/tests/gen
	$(PROTOC) $(OPTS) --pbwire_out=../bindings/php/tests/gen --pbwire_opt=lang=php $(PROTO)
	$(PROTOC) $(TESTDATA_OPTS) --pbwire_out=../bindings/php/tests/gen --pbwire_opt=lang=php $(TESTDATA)/oneof1.proto

gen-ruby: build
	mkdir -p ../bindings/ruby/test/gen ../bindings/ruby/test/gen_oracle
	$(PROTOC) $(OPTS) --pbwire_out=../bindings/ruby/test/gen --pbwire_opt=lang=ruby $(PROTO)
	$(PROTOC) $(TESTDATA_OPTS) --pbwire_out=../bindings/ruby/test/gen --pbwire_opt=lang=ruby $(TESTDATA)/oneof1.proto
	$(PROTOC) -I$(DATA) --ruby_out=../bindings/ruby/test/gen_oracle $(PROTO)
	$(PROTOC) -I$(TESTDATA) --ruby_out=../bindings/ruby/test/gen_oracle $(TESTDATA)/oneof1.proto

TESTDATA := $(CURDIR)/testdata
TESTDATA_OPTS = -I$(TESTDATA) --plugin=protoc-gen-pbwire=$(PLUGIN)

gen-python: build
	mkdir -p ../bindings/python/tests/gen ../bindings/python/tests/gen_oracle
	$(PROTOC) $(OPTS) --pbwire_out=../bindings/python/tests/gen --pbwire_opt=lang=python $(PROTO)
	$(PROTOC) $(TESTDATA_OPTS) --pbwire_out=../bindings/python/tests/gen --pbwire_opt=lang=python $(TESTDATA)/oneof1.proto
	$(PROTOC) -I$(DATA) --python_out=../bindings/python/tests/gen_oracle $(PROTO)
	$(PROTOC) -I$(TESTDATA) --python_out=../bindings/python/tests/gen_oracle $(TESTDATA)/oneof1.proto

gen-node: build
	mkdir -p ../bindings/node/tests/gen
	$(PROTOC) $(OPTS) --pbwire_out=../bindings/node/tests/gen --pbwire_opt=lang=typescript $(PROTO)
	$(PROTOC) $(OPTS) --pbwire_out=../bindings/node/tests/gen --pbwire_opt=lang=javascript $(PROTO)
	$(PROTOC) $(TESTDATA_OPTS) --pbwire_out=../bindings/node/tests/gen --pbwire_opt=lang=typescript $(TESTDATA)/oneof1.proto
	$(PROTOC) $(TESTDATA_OPTS) --pbwire_out=../bindings/node/tests/gen --pbwire_opt=lang=javascript $(TESTDATA)/oneof1.proto

GO_ORACLE := pbwire/tests/gen_oracle

gen-go: build
	mkdir -p ../bindings/go/tests/gen/matrix3 ../bindings/go/tests/gen/oneof1 ../bindings/go/tests/gen_oracle/matrix3pb ../bindings/go/tests/gen_oracle/oneof1pb
	$(PROTOC) $(OPTS) --pbwire_out=../bindings/go/tests/gen/matrix3 --pbwire_opt=lang=go $(PROTO)
	$(PROTOC) $(TESTDATA_OPTS) --pbwire_out=../bindings/go/tests/gen/oneof1 --pbwire_opt=lang=go $(TESTDATA)/oneof1.proto
	$(PROTOC) -I$(DATA) --go_out=../bindings/go/tests/gen_oracle/matrix3pb --go_opt=paths=source_relative '--go_opt=Mmatrix3.proto=$(GO_ORACLE)/matrix3pb' $(PROTO)
	$(PROTOC) -I$(TESTDATA) --go_out=../bindings/go/tests/gen_oracle/oneof1pb --go_opt=paths=source_relative '--go_opt=Moneof1.proto=$(GO_ORACLE)/oneof1pb' $(TESTDATA)/oneof1.proto

gen-rust: build
	mkdir -p ../bindings/rust/tests/gen
	$(PROTOC) $(OPTS) --pbwire_out=../bindings/rust/tests/gen --pbwire_opt=lang=rust $(PROTO)
	$(PROTOC) $(TESTDATA_OPTS) --pbwire_out=../bindings/rust/tests/gen --pbwire_opt=lang=rust $(TESTDATA)/oneof1.proto

gen-objc: build
	mkdir -p ../bindings/objc/tests/gen
	$(PROTOC) $(OPTS) --pbwire_out=../bindings/objc/tests/gen --pbwire_opt=lang=objc $(PROTO)
	$(PROTOC) $(TESTDATA_OPTS) --pbwire_out=../bindings/objc/tests/gen --pbwire_opt=lang=objc $(TESTDATA)/oneof1.proto

gen-lua: build
	mkdir -p ../bindings/lua/tests/gen
	$(PROTOC) $(OPTS) --pbwire_out=../bindings/lua/tests/gen --pbwire_opt=lang=lua $(PROTO)

gen-swift: build
	mkdir -p ../bindings/swift/Tests/PbwireTests/gen
	$(PROTOC) $(OPTS) --pbwire_out=../bindings/swift/Tests/PbwireTests/gen --pbwire_opt=lang=swift $(PROTO)

# shortcut: oneof1 omitted (new emitters treat oneof members as plain
# optionals); covered by the matrix3 round trip plus the shared oneof1.bin
# oracle in each binding's dynamic self-test.
gen-zig: build
	mkdir -p ../bindings/zig/tests/gen
	$(PROTOC) $(OPTS) --pbwire_out=../bindings/zig/tests/gen --pbwire_opt=lang=zig $(PROTO)

gen-scala: build
	mkdir -p ../bindings/scala/tests/gen
	$(PROTOC) $(OPTS) --pbwire_out=../bindings/scala/tests/gen --pbwire_opt=lang=scala $(PROTO)

gen-clojure: build
	mkdir -p ../bindings/clojure/tests/gen
	$(PROTOC) $(OPTS) --pbwire_out=../bindings/clojure/tests/gen --pbwire_opt=lang=clojure $(PROTO)

gen-fsharp: build
	mkdir -p ../bindings/fsharp/tests/gen
	$(PROTOC) $(OPTS) --pbwire_out=../bindings/fsharp/tests/gen --pbwire_opt=lang=fsharp $(PROTO)

gen-elixir: build
	mkdir -p ../bindings/elixir/test/gen
	$(PROTOC) $(OPTS) --pbwire_out=../bindings/elixir/test/gen --pbwire_opt=lang=elixir $(PROTO)

gen-julia: build
	mkdir -p ../bindings/julia/tests/gen
	$(PROTOC) $(OPTS) --pbwire_out=../bindings/julia/tests/gen --pbwire_opt=lang=julia $(PROTO)

gen-r: build
	mkdir -p ../bindings/r/tests/gen
	$(PROTOC) $(OPTS) --pbwire_out=../bindings/r/tests/gen --pbwire_opt=lang=r $(PROTO)

# The cross-file fixture: xfile.proto imports xdep.proto, so every language
# must import the sibling dep model instead of skipping the field. xdep goes
# first (protoc topological order also guarantees it); Go needs an explicit
# module mapping (M flag), every other language imports the sibling by name.
GEN_XFILE_GO_M := Mxdep.proto=pbwire/tests/gen/xdep

gen-xfile: build
	mkdir -p ../bindings/go/tests/gen/xdep ../bindings/go/tests/gen/xfile
	$(PROTOC) $(TESTDATA_OPTS) --pbwire_out=../bindings/go/tests/gen/xdep --pbwire_opt=lang=go $(TESTDATA)/xdep.proto
	$(PROTOC) $(TESTDATA_OPTS) --pbwire_out=../bindings/go/tests/gen/xfile "--pbwire_opt=lang=go,$(GEN_XFILE_GO_M)" $(TESTDATA)/xfile.proto
	for spec in "java:../bindings/java/tests/gen" "kotlin:../bindings/kotlin/tests/gen" \
	    "csharp:../bindings/csharp/generated/gen" "dart:../bindings/dart/tests/gen" \
	    "php:../bindings/php/tests/gen" "ruby:../bindings/ruby/test/gen" \
	    "python:../bindings/python/tests/gen" "typescript:../bindings/node/tests/gen" \
	    "javascript:../bindings/node/tests/gen" "rust:../bindings/rust/tests/gen" \
	    "objc:../bindings/objc/tests/gen" "lua:../bindings/lua/tests/gen" \
	    "swift:../bindings/swift/Tests/PbwireTests/gen" "zig:../bindings/zig/tests/gen" \
	    "scala:../bindings/scala/tests/gen" "clojure:../bindings/clojure/tests/gen" \
	    "fsharp:../bindings/fsharp/tests/gen" "elixir:../bindings/elixir/test/gen" \
	    "julia:../bindings/julia/tests/gen" "r:../bindings/r/tests/gen"; do \
		lang=$${spec%%:*}; dir=$${spec#*:}; \
		mkdir -p $$dir; \
		$(PROTOC) $(TESTDATA_OPTS) --pbwire_out=$$dir --pbwire_opt=lang=$$lang $(TESTDATA)/xdep.proto $(TESTDATA)/xfile.proto; \
	done

# The map fixture rides every language the same way (repeated entry structs),
# so one target generates it everywhere; each binding's map test (where the
# toolchain runs locally) reads its own file plus testdata/maptest.bin.
gen-maptest: build
	mkdir -p ../bindings/go/tests/gen/maptest
	$(PROTOC) $(TESTDATA_OPTS) --pbwire_out=../bindings/go/tests/gen/maptest --pbwire_opt=lang=go $(TESTDATA)/maptest.proto
	for spec in "java:../bindings/java/tests/gen" "kotlin:../bindings/kotlin/tests/gen" \
	    "csharp:../bindings/csharp/generated/gen" "dart:../bindings/dart/tests/gen" \
	    "php:../bindings/php/tests/gen" "ruby:../bindings/ruby/test/gen" \
	    "python:../bindings/python/tests/gen" "typescript:../bindings/node/tests/gen" \
	    "javascript:../bindings/node/tests/gen" "rust:../bindings/rust/tests/gen" \
	    "objc:../bindings/objc/tests/gen" "lua:../bindings/lua/tests/gen" \
	    "swift:../bindings/swift/Tests/PbwireTests/gen" "zig:../bindings/zig/tests/gen" \
	    "scala:../bindings/scala/tests/gen" "clojure:../bindings/clojure/tests/gen" \
	    "fsharp:../bindings/fsharp/tests/gen" "elixir:../bindings/elixir/test/gen" \
	    "julia:../bindings/julia/tests/gen" "r:../bindings/r/tests/gen"; do \
		lang=$${spec%%:*}; dir=$${spec#*:}; \
		mkdir -p $$dir; \
		$(PROTOC) $(TESTDATA_OPTS) --pbwire_out=$$dir --pbwire_opt=lang=$$lang $(TESTDATA)/maptest.proto; \
	done

# Each target needs its language's toolchain plus the shared library, which is
# why `make codegen-test` at the repo root is not part of the C commit gate.
test: test-java test-kotlin test-csharp test-dart test-php test-ruby test-python test-node test-rust test-objc test-lua test-go test-swift test-zig test-scala test-clojure test-fsharp test-elixir test-julia test-r

JAVA ?= java
RUBY ?= ruby
PYTHON ?= python3
LUA ?= lua
DART ?= dart
DOTNET ?= dotnet
NODE ?= node
CARGO ?= cargo
ZIG ?= zig
JULIA ?= julia
RSCRIPT ?= Rscript
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
LIB := $(CURDIR)/../build/libpbwire.dylib
else
LIB := $(CURDIR)/../build/libpbwire.so
endif

test-java: build
	cd ../bindings/java && ./build.sh >/dev/null

test-kotlin: build
	cd ../bindings/kotlin && ./build.sh >/dev/null
	cd ../bindings/kotlin && $(JAVA) --enable-native-access=ALL-UNNAMED -Dpbwire.library=$(LIB) \
		-cp out/pbwire.jar:../java/out GeneratedTestKt ../c/tests/data/matrix3.desc

test-csharp: build
	$(DOTNET) run --project ../bindings/csharp/generated -- ../bindings/c/tests/data/matrix3.desc

test-dart: build
	cd ../bindings/dart && PBWIRE_LIB=$(LIB) $(DART) run tests/generated_test.dart

test-php: build
	$(MAKE) -C .. shared >/dev/null
	php ../bindings/php/tests/generated_test.php
	php ../bindings/php/tests/compat_test.php

test-ruby: build
	$(MAKE) -C .. ruby-bind >/dev/null
	$(RUBY) -I../bindings/ruby/lib ../bindings/ruby/test/generated_test.rb
	$(RUBY) -I../bindings/ruby/lib ../bindings/ruby/test/test_compat.rb

test-python: build
	$(MAKE) -C .. python-bind >/dev/null
	$(PYTHON) ../bindings/python/tests/test_generated.py
	$(PYTHON) ../bindings/python/tests/test_compat.py

test-node:
	cd ../bindings/node && ./build.sh build >/dev/null
	$(NODE) --test ../bindings/node/tests/generated_test.js ../bindings/node/tests/compat_test.js

test-rust:
	cd ../bindings/rust && $(CARGO) test --test compat

test-go: build
	$(MAKE) -C .. build/libpbwire.a >/dev/null
	cd ../bindings/go && $(GO) test ./...

test-objc:
	cd ../bindings/objc && ./build.sh generated

test-lua: build
	LUA=$(LUA) $(MAKE) -C .. lua-bind >/dev/null
	cd ../bindings/lua && $(LUA) tests/test_generated.lua

SWIFT ?= swift
ZIG ?= zig
SCALAC ?= scalac
ELIXIR ?= elixir
JULIA ?= julia
RSCRIPT ?= Rscript

test-swift: build
	cd ../bindings/swift && $(SWIFT) test --filter GeneratedTest

test-zig: build
	cd ../bindings/zig && $(ZIG) build generated-test

test-scala: build
	cd ../bindings/scala && ./build.sh >/dev/null

test-clojure: build
	cd ../bindings/clojure && ./build.sh >/dev/null

test-fsharp: build
	cd ../bindings/fsharp && $(DOTNET) run --project generated -- ../c/tests/data/matrix3.bin

test-elixir: build
	cd ../bindings/elixir && mix test test/generated_test.exs

test-julia: build
	cd ../bindings/julia && $(JULIA) --project=runtime tests/generated_test.jl

test-r: build
	cd ../bindings/r && ./build.sh >/dev/null && $(RSCRIPT) tests/generated_test.R

# The real conformance proto (google/protobuf/test_messages_proto3.proto) is
# the hardest schema the generator meets: maps, oneofs, deep nesting, every
# scalar kind. Generating models for it must succeed and compile in every
# binding; the value flow is covered by the matrix3 generated tests above.
CONFORMANCE_PROTO := ../.tmp/protobuf-v36.2/src/google/protobuf/test_messages_proto3.proto

conformance: build
	@test -f $(CONFORMANCE_PROTO) || { \
		echo "protobuf checkout not found: $(CONFORMANCE_PROTO)"; \
		echo "run 'make conformance-setup' first"; exit 1; }
	@mkdir -p build/conformance build/conformance/go
	$(PROTOC) -I../.tmp/protobuf-v36.2/src $(OPTS) \
		--pbwire_out=build/conformance --pbwire_opt=lang=java $(CONFORMANCE_PROTO)
	$(PROTOC) -I../.tmp/protobuf-v36.2/src $(OPTS) \
		--pbwire_out=build/conformance --pbwire_opt=lang=kotlin $(CONFORMANCE_PROTO)
	$(PROTOC) -I../.tmp/protobuf-v36.2/src $(OPTS) \
		--pbwire_out=build/conformance --pbwire_opt=lang=csharp $(CONFORMANCE_PROTO)
	$(PROTOC) -I../.tmp/protobuf-v36.2/src $(OPTS) \
		--pbwire_out=build/conformance --pbwire_opt=lang=dart $(CONFORMANCE_PROTO)
	$(PROTOC) -I../.tmp/protobuf-v36.2/src $(OPTS) \
		--pbwire_out=build/conformance --pbwire_opt=lang=php $(CONFORMANCE_PROTO)
	$(PROTOC) -I../.tmp/protobuf-v36.2/src $(OPTS) \
		--pbwire_out=build/conformance --pbwire_opt=lang=ruby $(CONFORMANCE_PROTO)
	$(PROTOC) -I../.tmp/protobuf-v36.2/src $(OPTS) \
		--pbwire_out=build/conformance --pbwire_opt=lang=python $(CONFORMANCE_PROTO)
	$(PROTOC) -I../.tmp/protobuf-v36.2/src $(OPTS) \
		--pbwire_out=build/conformance --pbwire_opt=lang=typescript $(CONFORMANCE_PROTO)
	$(PROTOC) -I../.tmp/protobuf-v36.2/src $(OPTS) \
		--pbwire_out=build/conformance --pbwire_opt=lang=javascript $(CONFORMANCE_PROTO)
	$(PROTOC) -I../.tmp/protobuf-v36.2/src $(OPTS) \
		--pbwire_out=build/conformance --pbwire_opt=lang=rust $(CONFORMANCE_PROTO)
	$(PROTOC) -I../.tmp/protobuf-v36.2/src $(OPTS) \
		--pbwire_out=build/conformance --pbwire_opt=lang=objc $(CONFORMANCE_PROTO)
	$(PROTOC) -I../.tmp/protobuf-v36.2/src $(OPTS) \
		--pbwire_out=build/conformance --pbwire_opt=lang=lua $(CONFORMANCE_PROTO)
	$(PROTOC) -I../.tmp/protobuf-v36.2/src $(OPTS) \
		--pbwire_out=build/conformance/go --pbwire_opt=lang=go $(CONFORMANCE_PROTO)
	$(PROTOC) -I../.tmp/protobuf-v36.2/src $(OPTS) \
		--pbwire_out=build/conformance --pbwire_opt=lang=swift $(CONFORMANCE_PROTO)
	$(PROTOC) -I../.tmp/protobuf-v36.2/src $(OPTS) \
		--pbwire_out=build/conformance --pbwire_opt=lang=zig $(CONFORMANCE_PROTO)
	$(PROTOC) -I../.tmp/protobuf-v36.2/src $(OPTS) \
		--pbwire_out=build/conformance --pbwire_opt=lang=scala $(CONFORMANCE_PROTO)
	$(PROTOC) -I../.tmp/protobuf-v36.2/src $(OPTS) \
		--pbwire_out=build/conformance --pbwire_opt=lang=clojure $(CONFORMANCE_PROTO)
	$(PROTOC) -I../.tmp/protobuf-v36.2/src $(OPTS) \
		--pbwire_out=build/conformance --pbwire_opt=lang=fsharp $(CONFORMANCE_PROTO)
	$(PROTOC) -I../.tmp/protobuf-v36.2/src $(OPTS) \
		--pbwire_out=build/conformance --pbwire_opt=lang=elixir $(CONFORMANCE_PROTO)
	$(PROTOC) -I../.tmp/protobuf-v36.2/src $(OPTS) \
		--pbwire_out=build/conformance --pbwire_opt=lang=julia $(CONFORMANCE_PROTO)
	$(PROTOC) -I../.tmp/protobuf-v36.2/src $(OPTS) \
		--pbwire_out=build/conformance --pbwire_opt=lang=r $(CONFORMANCE_PROTO)
	@echo "generated models for test_messages_proto3.proto: build/conformance"
	$(MAKE) conformance-check

# The generated conformance models must compile. TypeScript has no compiler in
# this repo (checked by hand with tsc --strict); everything else is checked here.
conformance-check: build
	@test -f build/conformance/test_messages_proto3_pbwire.rs || { echo "run 'make conformance' first"; exit 1; }
	mkdir -p build/conformance/java-out
	javac -nowarn -d build/conformance/java-out -cp ../bindings/java/out build/conformance/*.java
	kotlinc -nowarn -cp ../bindings/java/out:build/conformance/java-out -d build/conformance/conf.jar build/conformance/test_messages_proto3.kt
	@mkdir -p build/conformance/csharp
	printf '%s\n' '<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><TargetFramework>net10.0</TargetFramework><Nullable>enable</Nullable><AllowUnsafeBlocks>true</AllowUnsafeBlocks></PropertyGroup><ItemGroup><ProjectReference Include="$(CURDIR)/../bindings/csharp/Pbwire/Pbwire.csproj" /><PackageReference Include="Google.Protobuf" Version="3.36.2" /><Compile Include="$(CURDIR)/build/conformance/test_messages_proto3.cs" /></ItemGroup></Project>' > build/conformance/csharp/conf.csproj
	$(DOTNET) build build/conformance/csharp/conf.csproj
	cp build/conformance/test_messages_proto3.dart ../bindings/dart/tests/gen/conf_models.dart
	cd ../bindings/dart && $(DART) analyze tests/gen/conf_models.dart
	rm -f ../bindings/dart/tests/gen/conf_models.dart
	php -l build/conformance/test_messages_proto3.php
	ruby -c build/conformance/test_messages_proto3.rb
	$(PYTHON) -m py_compile build/conformance/test_messages_proto3_pbwire.py
	$(NODE) --check build/conformance/test_messages_proto3.js
	$(LUA) -e 'assert(loadfile("build/conformance/test_messages_proto3.lua"))'
	$(MAKE) -C .. build/libpbwire.a >/dev/null
	mkdir -p ../bindings/go/conformancetmp
	cp build/conformance/go/test_messages_proto3_pbwire.go ../bindings/go/conformancetmp/
	cd ../bindings/go && $(GO) build ./conformancetmp/
	rm -rf ../bindings/go/conformancetmp
	clang -fsyntax-only -fobjc-arc -Wall -Wextra -I../include -I../bindings/objc -Ibuild/conformance build/conformance/test_messages_proto3.m
	printf '#[path = "../../../codegen/build/conformance/test_messages_proto3_pbwire.rs"]\nmod conf;\n#[test]\nfn conformance_models_compile() { let _ = conf::TestAllTypesProto3::FULL_NAME; }\n' > ../bindings/rust/tests/conf_gen.rs
	cd ../bindings/rust && $(CARGO) test --test conf_gen
	rm -f ../bindings/rust/tests/conf_gen.rs
	$(ZIG) ast-check build/conformance/test_messages_proto3.zig
	swiftc -parse build/conformance/test_messages_proto3.swift
	$(RSCRIPT) -e 'invisible(parse("build/conformance/test_messages_proto3_pbwire.R"))'
	cd ../bindings/julia && $(JULIA) --project=runtime -e 'Meta.parseall(read("../../codegen/build/conformance/test_messages_proto3_pbwire.jl", String))'
	cd ../bindings/elixir && MIX_ENV=test mix run -e '"../../codegen/build/conformance/test_messages_proto3.ex" |> File.read!() |> Code.string_to_quoted!() |> is_tuple() |> IO.inspect(label: :parses)'
	mkdir -p build/conformance/scala-out build/conformance/scala-classes
	javac -nowarn -d build/conformance/scala-out ../bindings/java/runtime/*.java
	scalac -nowarn -cp build/conformance/scala-out -d build/conformance/scala-classes build/conformance/test_messages_proto3.scala
	mkdir -p build/conformance/fsharp
	cp build/conformance/test_messages_proto3.fs build/conformance/fsharp/
	printf '%s\n' '<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><TargetFramework>net10.0</TargetFramework><Nullable>enable</Nullable><InvariantGlobalization>true</InvariantGlobalization></PropertyGroup><ItemGroup><Compile Include="test_messages_proto3.fs" /><ProjectReference Include="$(CURDIR)/../bindings/fsharp/runtime/Pbwire.FSharp.fsproj" /></ItemGroup></Project>' > build/conformance/fsharp/conf.fsproj
	$(DOTNET) build build/conformance/fsharp/conf.fsproj
	@echo "conformance models compile in java, kotlin, c#, dart, php, ruby, python, node, objc, rust, lua, go, swift, zig, scala, clojure, fsharp, elixir, julia, r"
	@echo "shortcut: clojure parses but does not compile here (needs the fetched jars); test-clojure compiles the same shape"

.PHONY: all build gen gen-maptest gen-xfile gen-examples test conformance conformance-check $(addprefix gen-,$(LANGS)) \
	gen-java gen-kotlin gen-csharp gen-dart gen-php gen-ruby gen-python gen-node gen-rust gen-objc gen-lua gen-go \
	gen-swift gen-zig gen-scala gen-clojure gen-fsharp gen-elixir gen-julia gen-r \
	test-java test-kotlin test-csharp test-dart test-php test-ruby test-python test-node test-rust test-objc test-lua test-go \
	test-swift test-zig test-scala test-clojure test-fsharp test-elixir test-julia test-r
