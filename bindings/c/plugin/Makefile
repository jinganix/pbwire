# protoc-gen-pbwire-c: protoc plugin generating C structs + layout tables
# for the ../runtime struct engine. Needs Go and protoc, so it stays out of
# the root `make test` gate (which is C99-only): run these targets by hand.
#
#   make build   # compile the plugin + dump helper into build/
#   make test    # golden tests (protoc-free: replay checked-in dumps)
#   make regen   # regenerate the pinned headers + descriptor sets (needs protoc)
#   make dump    # re-capture testdata dumps (needs protoc; only for new cases)
GO ?= go
PROTOC ?= protoc
ROOT := $(CURDIR)/../../..
BIN := $(CURDIR)/build/protoc-gen-pbwire-c
DUMP := $(CURDIR)/build/protoc-dump

build: $(BIN) $(DUMP)

$(BIN): go.mod go.sum $(shell find cmd internal -type f -name '*.go')
	@mkdir -p build
	$(GO) build -o $(BIN) ./cmd/protoc-gen-pbwire-c

$(DUMP): go.mod go.sum $(shell find cmd internal -type f -name '*.go')
	@mkdir -p build
	$(GO) build -o $(DUMP) ./cmd/protoc-dump

test: build
	$(GO) test ./...

# Regenerate the pinned headers the C example/tests build against, plus
# the FileDescriptorSets they load at runtime (kept next to the .protos,
# not embedded). The .txt/.bin reference vectors are NOT regenerated here:
# edit the .txt by hand, then re-encode it with protoc --encode (see the
# header comment in bindings/c/tests/data/matrix3.proto).
regen: build
	@rm -rf build/regen && mkdir -p build/regen ../tests/gen
	PATH="$(CURDIR)/build:$$PATH" $(PROTOC) -I$(ROOT) \
		--plugin=protoc-gen-pbwire-c=$(BIN) \
		--pbwire-c_out=$(CURDIR)/build/regen \
		examples/addressbook.proto
	cp build/regen/addressbook.h ../examples/gen/addressbook.h
	$(PROTOC) -I$(ROOT) --include_imports \
		--descriptor_set_out=$(ROOT)/examples/addressbook.desc \
		examples/addressbook.proto
	PATH="$(CURDIR)/build:$$PATH" $(PROTOC) -I$(ROOT) \
		--plugin=protoc-gen-pbwire-c=$(BIN) \
		--pbwire-c_out=$(CURDIR)/build/regen \
		bindings/c/tests/data/matrix3.proto bindings/c/tests/data/matrix2.proto bindings/c/tests/data/maptest.proto bindings/c/tests/data/grouptest.proto bindings/c/tests/data/oneoftest.proto bindings/c/tests/data/extensiontest.proto
	cp build/regen/matrix3.h build/regen/matrix2.h build/regen/maptest.h build/regen/grouptest.h build/regen/oneoftest.h build/regen/extensiontest.h ../tests/gen/
	$(MAKE) regen-recur
	$(PROTOC) -I$(ROOT) --include_imports \
		--descriptor_set_out=$(ROOT)/bindings/c/tests/data/matrix3.desc \
		bindings/c/tests/data/matrix3.proto
	$(PROTOC) -I$(ROOT) --include_imports \
		--descriptor_set_out=$(ROOT)/bindings/c/tests/data/matrix2.desc \
		bindings/c/tests/data/matrix2.proto
	$(PROTOC) -I$(ROOT) --include_imports \
		--descriptor_set_out=$(ROOT)/bindings/c/tests/data/maptest.desc \
		bindings/c/tests/data/maptest.proto
	$(PROTOC) -I$(ROOT) --include_imports \
		--descriptor_set_out=$(ROOT)/bindings/c/tests/data/grouptest.desc \
		bindings/c/tests/data/grouptest.proto
	$(PROTOC) -I$(ROOT) --include_imports \
		--descriptor_set_out=$(ROOT)/bindings/c/tests/data/oneoftest.desc \
		bindings/c/tests/data/oneoftest.proto
	$(PROTOC) -I$(ROOT) --include_imports \
		--descriptor_set_out=$(ROOT)/bindings/c/tests/data/extensiontest.desc \
		bindings/c/tests/data/extensiontest.proto
	@echo "regenerated headers and descriptor sets; run (cd ../../.. && make test)"

# Re-capture the CodeGeneratorRequest dumps golden tests replay.
dump: build
	PATH="$(CURDIR)/build:$$PATH" $(PROTOC) -I$(ROOT) \
		--plugin=protoc-gen-dump=$(DUMP) \
		--dump_out=$(CURDIR)/testdata/c/addressbook \
		examples/addressbook.proto
	PATH="$(CURDIR)/build:$$PATH" $(PROTOC) -I$(ROOT) \
		--plugin=protoc-gen-dump=$(DUMP) \
		--dump_out=$(CURDIR)/testdata/c/matrix3 \
		bindings/c/tests/data/matrix3.proto
	PATH="$(CURDIR)/build:$$PATH" $(PROTOC) -I$(ROOT) \
		--plugin=protoc-gen-dump=$(DUMP) \
		--dump_out=$(CURDIR)/testdata/c/matrix2 \
		bindings/c/tests/data/matrix2.proto
	PATH="$(CURDIR)/build:$$PATH" $(PROTOC) -I$(ROOT) \
		--plugin=protoc-gen-dump=$(DUMP) \
		--dump_out=$(CURDIR)/testdata/c/maptest \
		bindings/c/tests/data/maptest.proto
	PATH="$(CURDIR)/build:$$PATH" $(PROTOC) -I$(ROOT) \
		--plugin=protoc-gen-dump=$(DUMP) \
		--dump_out=$(CURDIR)/testdata/c/grouptest \
		bindings/c/tests/data/grouptest.proto
	PATH="$(CURDIR)/build:$$PATH" $(PROTOC) -I$(ROOT) \
		--plugin=protoc-gen-dump=$(DUMP) \
		--dump_out=$(CURDIR)/testdata/c/oneoftest \
		bindings/c/tests/data/oneoftest.proto
	PATH="$(CURDIR)/build:$$PATH" $(PROTOC) -I$(ROOT) \
		--plugin=protoc-gen-dump=$(DUMP) \
		--dump_out=$(CURDIR)/testdata/c/extensiontest \
		bindings/c/tests/data/extensiontest.proto
	PATH="$(CURDIR)/build:$$PATH" $(PROTOC) -I$(ROOT) \
		--plugin=protoc-gen-dump=$(DUMP) \
		--dump_out=$(CURDIR)/testdata/c/recur \
		bindings/c/plugin/testdata/proto/recur.proto

# Refresh the cross-file/recursive fixture: its dump, the descriptor sets its
# headers name, and the pinned headers the golden test compares against.
regen-recur: build
	PATH="$(CURDIR)/build:$$PATH" $(PROTOC) -I$(ROOT) \
		--plugin=protoc-gen-dump=$(DUMP) \
		--dump_out=$(CURDIR)/testdata/c/recur \
		bindings/c/plugin/testdata/proto/recur.proto
	$(PROTOC) -I$(ROOT) --include_imports \
		--descriptor_set_out=$(CURDIR)/testdata/proto/recur.desc \
		bindings/c/plugin/testdata/proto/recur.proto
	$(PROTOC) -I$(ROOT) --include_imports \
		--descriptor_set_out=$(CURDIR)/testdata/proto/dep.desc \
		bindings/c/plugin/testdata/proto/dep.proto
	$(GO) run ./cmd/regen-headers testdata/c/recur/test.dump testdata/c/recur
	@echo "refreshed testdata/c/recur; run 'go test ./...'"

.PHONY: build test regen dump regen-recur
