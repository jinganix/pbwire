# Typed message models: one protoc plugin, one model per language.
#
#   make build   build protoc-gen-pbwire-typed
#   make gen     regenerate the committed models (needs protoc)
#   make test    run every binding's typed test (needs each toolchain)
#
# The models are committed next to each binding's tests so the tests stay
# protoc-free; `make gen` is how they get refreshed.
PROTOC ?= protoc
GO ?= go
PLUGIN := build/protoc-gen-pbwire-typed
DATA := ../c/tests/data
PROTO := $(DATA)/matrix3.proto
OPTS = -I$(DATA) --plugin=protoc-gen-pbwire-typed=$(PLUGIN)

all: build

build:
	$(GO) build -o $(PLUGIN) ./cmd/protoc-gen-pbwire-typed

gen: gen-java gen-kotlin gen-csharp gen-dart gen-php gen-ruby gen-python gen-node gen-rust gen-objc gen-examples

# The examples all speak the C example's addressbook.proto: same schema, same
# bytes, one typed walkthrough per language.
EXAMPLE := ../../examples/addressbook.proto
EXAMPLE_OPTS = -I../../examples --plugin=protoc-gen-pbwire-typed=$(PLUGIN)

gen-examples: build
	mkdir -p ../java/examples/gen ../kotlin/examples/gen ../csharp/examples/gen \
		../dart/examples/gen ../php/examples/gen ../ruby/examples/gen \
		../python/examples/gen ../node/examples/gen ../rust/examples/gen \
		../objc/examples/gen
	$(PROTOC) $(EXAMPLE_OPTS) --pbwire-typed_out=../java/examples/gen --pbwire-typed_opt=lang=java $(EXAMPLE)
	$(PROTOC) $(EXAMPLE_OPTS) --pbwire-typed_out=../kotlin/examples/gen --pbwire-typed_opt=lang=kotlin $(EXAMPLE)
	$(PROTOC) $(EXAMPLE_OPTS) --pbwire-typed_out=../csharp/examples/gen --pbwire-typed_opt=lang=csharp $(EXAMPLE)
	$(PROTOC) $(EXAMPLE_OPTS) --pbwire-typed_out=../dart/examples/gen --pbwire-typed_opt=lang=dart $(EXAMPLE)
	$(PROTOC) $(EXAMPLE_OPTS) --pbwire-typed_out=../php/examples/gen --pbwire-typed_opt=lang=php $(EXAMPLE)
	$(PROTOC) $(EXAMPLE_OPTS) --pbwire-typed_out=../ruby/examples/gen --pbwire-typed_opt=lang=ruby $(EXAMPLE)
	$(PROTOC) $(EXAMPLE_OPTS) --pbwire-typed_out=../python/examples/gen --pbwire-typed_opt=lang=python $(EXAMPLE)
	$(PROTOC) $(EXAMPLE_OPTS) --pbwire-typed_out=../node/examples/gen --pbwire-typed_opt=lang=javascript $(EXAMPLE)
	$(PROTOC) $(EXAMPLE_OPTS) --pbwire-typed_out=../node/examples/gen --pbwire-typed_opt=lang=typescript $(EXAMPLE)
	$(PROTOC) $(EXAMPLE_OPTS) --pbwire-typed_out=../rust/examples/gen --pbwire-typed_opt=lang=rust $(EXAMPLE)
	$(PROTOC) $(EXAMPLE_OPTS) --pbwire-typed_out=../objc/examples/gen --pbwire-typed_opt=lang=objc $(EXAMPLE)

gen-java: build
	mkdir -p ../java/tests/gen
	$(PROTOC) $(OPTS) --pbwire-typed_out=../java/tests/gen --pbwire-typed_opt=lang=java $(PROTO)

gen-kotlin: build
	mkdir -p ../kotlin/tests/gen
	$(PROTOC) $(OPTS) --pbwire-typed_out=../kotlin/tests/gen --pbwire-typed_opt=lang=kotlin $(PROTO)

gen-csharp: build
	mkdir -p ../csharp/typed/gen
	$(PROTOC) $(OPTS) --pbwire-typed_out=../csharp/typed/gen --pbwire-typed_opt=lang=csharp $(PROTO)

gen-dart: build
	mkdir -p ../dart/tests/gen
	$(PROTOC) $(OPTS) --pbwire-typed_out=../dart/tests/gen --pbwire-typed_opt=lang=dart $(PROTO)

gen-php: build
	mkdir -p ../php/tests/gen
	$(PROTOC) $(OPTS) --pbwire-typed_out=../php/tests/gen --pbwire-typed_opt=lang=php $(PROTO)

gen-ruby: build
	mkdir -p ../ruby/test/gen
	$(PROTOC) $(OPTS) --pbwire-typed_out=../ruby/test/gen --pbwire-typed_opt=lang=ruby $(PROTO)

gen-python: build
	mkdir -p ../python/tests/gen
	$(PROTOC) $(OPTS) --pbwire-typed_out=../python/tests/gen --pbwire-typed_opt=lang=python $(PROTO)

gen-node: build
	mkdir -p ../node/tests/gen
	$(PROTOC) $(OPTS) --pbwire-typed_out=../node/tests/gen --pbwire-typed_opt=lang=typescript $(PROTO)
	$(PROTOC) $(OPTS) --pbwire-typed_out=../node/tests/gen --pbwire-typed_opt=lang=javascript $(PROTO)

gen-rust: build
	mkdir -p ../rust/tests/gen
	$(PROTOC) $(OPTS) --pbwire-typed_out=../rust/tests/gen --pbwire-typed_opt=lang=rust $(PROTO)

gen-objc: build
	mkdir -p ../objc/tests/gen
	$(PROTOC) $(OPTS) --pbwire-typed_out=../objc/tests/gen --pbwire-typed_opt=lang=objc $(PROTO)

# Each target needs its language's toolchain plus the shared library, which is
# why `make typed-test` at the repo root is not part of the C commit gate.
test: test-java test-kotlin test-csharp test-dart test-php test-ruby test-python test-node test-rust test-objc

JAVA ?= java
RUBY ?= ruby
PYTHON ?= python3
DART ?= dart
DOTNET ?= dotnet
NODE ?= node
CARGO ?= cargo
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
LIB := $(CURDIR)/../../build/libpbwire.dylib
else
LIB := $(CURDIR)/../../build/libpbwire.so
endif

test-java: build
	cd ../java && ./build.sh >/dev/null
	cd ../java && $(JAVA) --enable-native-access=ALL-UNNAMED -Dpbwire.library=$(LIB) \
		-cp out TestTyped ../c/tests/data/matrix3.desc ../c/tests/data/matrix3.bin

test-kotlin: build
	cd ../kotlin && ./build.sh >/dev/null
	cd ../kotlin && $(JAVA) --enable-native-access=ALL-UNNAMED -Dpbwire.library=$(LIB) \
		-cp out/pbwire.jar:../java/out TypedTestKt ../c/tests/data/matrix3.desc

test-csharp: build
	$(DOTNET) run --project ../csharp/typed -- ../c/tests/data/matrix3.desc

test-dart: build
	cd ../dart && PBWIRE_LIB=$(LIB) $(DART) run tests/typed_test.dart

test-php: build
	$(MAKE) -C ../.. shared >/dev/null
	php ../php/tests/typed_test.php

test-ruby: build
	$(MAKE) -C ../.. ruby-bind >/dev/null
	$(RUBY) -I../ruby/lib ../ruby/test/typed_test.rb

test-python: build
	$(MAKE) -C ../.. python-bind >/dev/null
	$(PYTHON) ../python/tests/test_typed.py

test-node:
	cd ../node && ./build.sh build >/dev/null
	$(NODE) --test ../node/tests/typed_test.js

test-rust:
	cd ../rust && $(CARGO) test --test typed

test-objc:
	cd .. && ./objc/build.sh typed

# The real conformance proto (google/protobuf/test_messages_proto3.proto) is
# the hardest schema the generator meets: maps, oneofs, deep nesting, every
# scalar kind. Generating models for it must succeed and compile in every
# binding; the value flow is covered by the matrix3 typed tests above.
CONFORMANCE_PROTO := ../../.tmp/protobuf-v36.2/src/google/protobuf/test_messages_proto3.proto

conformance: build
	@test -f $(CONFORMANCE_PROTO) || { \
		echo "protobuf checkout not found: $(CONFORMANCE_PROTO)"; \
		echo "run 'make conformance-setup' first"; exit 1; }
	@mkdir -p build/conformance
	$(PROTOC) -I../../.tmp/protobuf-v36.2/src $(OPTS) \
		--pbwire-typed_out=build/conformance --pbwire-typed_opt=lang=java $(CONFORMANCE_PROTO)
	$(PROTOC) -I../../.tmp/protobuf-v36.2/src $(OPTS) \
		--pbwire-typed_out=build/conformance --pbwire-typed_opt=lang=kotlin $(CONFORMANCE_PROTO)
	$(PROTOC) -I../../.tmp/protobuf-v36.2/src $(OPTS) \
		--pbwire-typed_out=build/conformance --pbwire-typed_opt=lang=csharp $(CONFORMANCE_PROTO)
	$(PROTOC) -I../../.tmp/protobuf-v36.2/src $(OPTS) \
		--pbwire-typed_out=build/conformance --pbwire-typed_opt=lang=dart $(CONFORMANCE_PROTO)
	$(PROTOC) -I../../.tmp/protobuf-v36.2/src $(OPTS) \
		--pbwire-typed_out=build/conformance --pbwire-typed_opt=lang=php $(CONFORMANCE_PROTO)
	$(PROTOC) -I../../.tmp/protobuf-v36.2/src $(OPTS) \
		--pbwire-typed_out=build/conformance --pbwire-typed_opt=lang=ruby $(CONFORMANCE_PROTO)
	$(PROTOC) -I../../.tmp/protobuf-v36.2/src $(OPTS) \
		--pbwire-typed_out=build/conformance --pbwire-typed_opt=lang=python $(CONFORMANCE_PROTO)
	$(PROTOC) -I../../.tmp/protobuf-v36.2/src $(OPTS) \
		--pbwire-typed_out=build/conformance --pbwire-typed_opt=lang=typescript $(CONFORMANCE_PROTO)
	$(PROTOC) -I../../.tmp/protobuf-v36.2/src $(OPTS) \
		--pbwire-typed_out=build/conformance --pbwire-typed_opt=lang=javascript $(CONFORMANCE_PROTO)
	$(PROTOC) -I../../.tmp/protobuf-v36.2/src $(OPTS) \
		--pbwire-typed_out=build/conformance --pbwire-typed_opt=lang=rust $(CONFORMANCE_PROTO)
	$(PROTOC) -I../../.tmp/protobuf-v36.2/src $(OPTS) \
		--pbwire-typed_out=build/conformance --pbwire-typed_opt=lang=objc $(CONFORMANCE_PROTO)
	@echo "typed models for test_messages_proto3.proto: build/conformance"
	$(MAKE) conformance-check

# The generated conformance models must compile. TypeScript has no compiler in
# this repo (checked by hand with tsc --strict); everything else is checked here.
conformance-check: build
	@test -f build/conformance/test_messages_proto3_pbwire.rs || { echo "run 'make conformance' first"; exit 1; }
	mkdir -p build/conformance/java-out
	javac -nowarn -d build/conformance/java-out -cp ../java/out build/conformance/*.java
	kotlinc -nowarn -cp ../java/out -d build/conformance/conf.jar build/conformance/test_messages_proto3.kt
	@mkdir -p build/conformance/csharp
	printf '%s\n' '<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><TargetFramework>net10.0</TargetFramework><Nullable>enable</Nullable><AllowUnsafeBlocks>true</AllowUnsafeBlocks></PropertyGroup><ItemGroup><ProjectReference Include="$(CURDIR)/../csharp/Pbwire/Pbwire.csproj" /><Compile Include="$(CURDIR)/build/conformance/test_messages_proto3.cs" /></ItemGroup></Project>' > build/conformance/csharp/conf.csproj
	$(DOTNET) build build/conformance/csharp/conf.csproj
	cp build/conformance/test_messages_proto3.dart ../dart/tests/gen/conf_models.dart
	cd ../dart && $(DART) analyze tests/gen/conf_models.dart
	rm -f ../dart/tests/gen/conf_models.dart
	php -l build/conformance/test_messages_proto3.php
	ruby -c build/conformance/test_messages_proto3.rb
	$(PYTHON) -m py_compile build/conformance/test_messages_proto3_pbwire.py
	$(NODE) --check build/conformance/test_messages_proto3.js
	clang -fsyntax-only -fobjc-arc -Wall -Wextra -I../../include -I../objc -Ibuild/conformance build/conformance/test_messages_proto3.m
	printf '#[path = "../../typed/build/conformance/test_messages_proto3_pbwire.rs"]\nmod conf;\n#[test]\nfn conformance_models_compile() { let _ = conf::TestAllTypesProto3::FULL_NAME; }\n' > ../rust/tests/conf_gen.rs
	cd ../rust && $(CARGO) test --test conf_gen
	rm -f ../rust/tests/conf_gen.rs
	@echo "conformance models compile in java, kotlin, c#, dart, php, ruby, python, node, objc, rust"

.PHONY: all build gen gen-examples test conformance conformance-check $(addprefix gen-,$(LANGS)) \
	gen-java gen-kotlin gen-csharp gen-dart gen-php gen-ruby gen-python gen-node gen-rust gen-objc \
	test-java test-kotlin test-csharp test-dart test-php test-ruby test-python test-node test-rust test-objc
