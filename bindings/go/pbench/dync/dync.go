// Package dync is the dynamic C core arm: the struct moves into a pbwire C
// tree through the shipped ImportModel converter and back through ExportModel.
// Encoding and decoding run in the shared C core over cgo. No test-only glue:
// every conversion here is public API with its own contract and tests.
package dync

import (
	"fmt"
	"os"
	"path/filepath"
	"runtime"

	"pbwire/pbench/dync/dynamic"
	"pbwire/tests/gen/matrix3"
)

// Codec owns the loaded schema and the message type. It is not safe for
// concurrent use: the C core confines a schema to one thread.
type Codec struct {
	schema *dynamic.Schema
	typ    *dynamic.MessageType
}

func descriptorPath() string {
	_, file, _, _ := runtime.Caller(0)
	return filepath.Join(filepath.Dir(file), "..", "..", "runtime", "testdata", "matrix3.desc")
}

// New loads the pinned descriptor set.
func New() (*Codec, error) {
	raw, err := os.ReadFile(descriptorPath())
	if err != nil {
		return nil, fmt.Errorf("dync: descriptor: %w", err)
	}
	s, err := dynamic.Load(raw)
	if err != nil {
		return nil, fmt.Errorf("dync: load: %w", err)
	}
	typ, err := s.Message("structtest.Scalars3")
	if err != nil {
		s.Close()
		return nil, fmt.Errorf("dync: type: %w", err)
	}
	return &Codec{schema: s, typ: typ}, nil
}

// Name reports the arm.
func (c *Codec) Name() string { return "dync" }

// Close releases the schema and the message type.
func (c *Codec) Close() {
	c.typ.Close()
	c.schema.Close()
}

// Marshal copies the struct into a C tree and encodes it.
func (c *Codec) Marshal(m *matrix3.Scalars3) ([]byte, error) {
	tree, err := c.typ.ImportModel(m, matrix3.Scalars3Meta)
	if err != nil {
		return nil, err
	}
	defer tree.Close()
	return tree.Encode()
}

// Unmarshal decodes into a C tree and copies it back into a struct.
func (c *Codec) Unmarshal(wire []byte) (*matrix3.Scalars3, error) {
	tree, err := c.typ.Decode(wire)
	if err != nil {
		return nil, err
	}
	defer tree.Close()
	var out matrix3.Scalars3
	if err := dynamic.ExportModel(tree, &out, matrix3.Scalars3Meta); err != nil {
		return nil, err
	}
	return &out, nil
}
