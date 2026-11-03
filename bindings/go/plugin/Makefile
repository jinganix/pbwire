# protoc-gen-pbwire-go: protoc plugin generating Go structs + straight-line
# size/encode/decode methods for the Go runtime. Needs Go and protoc, so it
# stays out of the root `make test` gate: run these targets by hand.
#
#   make build   # compile the plugin + dump helper into build/
#   make test    # golden tests (protoc-free: replay checked-in dumps)
#   make regen   # regenerate the pinned Go files (needs protoc)
#   make dump    # re-capture testdata dumps (needs protoc; only for new cases)
GO ?= go
PROTOC ?= protoc
ROOT := $(CURDIR)/../../..
BIN := $(CURDIR)/build/protoc-gen-pbwire-go
DUMP := $(CURDIR)/build/protoc-dump
GOSRC := $(shell find . -type f -name '*.go')

build: $(BIN) $(DUMP)

$(BIN): go.mod go.sum $(GOSRC)
	@mkdir -p build
	$(GO) build -o $(BIN) ./cmd/protoc-gen-pbwire-go

$(DUMP): go.mod go.sum $(GOSRC)
	@mkdir -p build
	$(GO) build -o $(DUMP) ./cmd/protoc-dump

test: build
	$(GO) test ./...

# Regenerate the pinned Go files the examples/tests build against. The
# matrix protos, their reference bytes and the addressbook schema stay in
# their C/example homes: the Go binding compiles the same schemas, so there
# is one copy of each fixture.
regen: build
	PATH="$(CURDIR)/build:$$PATH" $(PROTOC) \
		-I$(ROOT)/bindings/c/tests/data \
		--plugin=protoc-gen-pbwire-go=$(BIN) \
		--pbwire-go_out=$(ROOT)/bindings/go/tests/gen/structtest \
		$(ROOT)/bindings/c/tests/data/matrix3.proto $(ROOT)/bindings/c/tests/data/matrix2.proto
	PATH="$(CURDIR)/build:$$PATH" $(PROTOC) \
		-I$(ROOT)/bindings/go/tests/data \
		--plugin=protoc-gen-pbwire-go=$(BIN) \
		--pbwire-go_out=$(ROOT)/bindings/go/tests/gen/edgetest \
		$(ROOT)/bindings/go/tests/data/edges.proto
	@mkdir -p $(ROOT)/bindings/go/examples/addressbook/tutorial
	PATH="$(CURDIR)/build:$$PATH" $(PROTOC) \
		-I$(ROOT)/examples \
		--plugin=protoc-gen-pbwire-go=$(BIN) \
		--pbwire-go_out=$(ROOT)/bindings/go/examples/addressbook/tutorial \
		$(ROOT)/examples/addressbook.proto

# Re-capture the CodeGeneratorRequest dumps the golden tests replay.
dump: build
	@mkdir -p testdata/addressbook testdata/matrix3 testdata/matrix2 testdata/edges
	PATH="$(CURDIR)/build:$$PATH" $(PROTOC) -I$(ROOT)/examples \
		--plugin=protoc-gen-dump=$(DUMP) \
		--dump_out=$(CURDIR)/testdata/addressbook $(ROOT)/examples/addressbook.proto
	PATH="$(CURDIR)/build:$$PATH" $(PROTOC) -I$(ROOT)/bindings/c/tests/data \
		--plugin=protoc-gen-dump=$(DUMP) \
		--dump_out=$(CURDIR)/testdata/matrix3 $(ROOT)/bindings/c/tests/data/matrix3.proto
	PATH="$(CURDIR)/build:$$PATH" $(PROTOC) -I$(ROOT)/bindings/c/tests/data \
		--plugin=protoc-gen-dump=$(DUMP) \
		--dump_out=$(CURDIR)/testdata/matrix2 $(ROOT)/bindings/c/tests/data/matrix2.proto
	PATH="$(CURDIR)/build:$$PATH" $(PROTOC) -I$(ROOT)/bindings/go/tests/data \
		--plugin=protoc-gen-dump=$(DUMP) \
		--dump_out=$(CURDIR)/testdata/edges $(ROOT)/bindings/go/tests/data/edges.proto

fmt:
	$(GO) fmt ./...

.PHONY: build test regen dump fmt
