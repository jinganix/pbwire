// Package dyngo is the dynamic Go core arm: the struct moves into a codec
// DMsg value tree through the shipped ImportModel converter and back through
// ExportModel, with no cgo. No test-only glue: every conversion here is
// public API with its own contract and tests.
package dyngo

import (
	"fmt"
	"os"
	"path/filepath"
	"runtime"

	"pbwire/pbench/dyngo/codec"
	"pbwire/tests/gen/matrix3"
)

// Codec owns the loaded schema and the resolved dynamic type.
type Codec struct {
	schema *codec.Schema
	typ    *codec.DType
}

func descriptorPath() string {
	_, file, _, _ := runtime.Caller(0)
	return filepath.Join(filepath.Dir(file), "codec", "testdata", "matrix3.desc")
}

// New loads the pinned descriptor set and resolves the message type.
func New() (*Codec, error) {
	raw, err := os.ReadFile(descriptorPath())
	if err != nil {
		return nil, fmt.Errorf("dyngo: descriptor: %w", err)
	}
	s, err := codec.Load(raw)
	if err != nil {
		return nil, fmt.Errorf("dyngo: load: %w", err)
	}
	typ, err := s.Type("structtest.Scalars3")
	if err != nil {
		return nil, fmt.Errorf("dyngo: type: %w", err)
	}
	return &Codec{schema: s, typ: typ}, nil
}

// Name reports the arm.
func (c *Codec) Name() string { return "dyngo" }

// Marshal copies the struct into a value tree and encodes it.
func (c *Codec) Marshal(m *matrix3.Scalars3) ([]byte, error) {
	tree, err := c.typ.ImportModel(m, matrix3.Scalars3Meta)
	if err != nil {
		return nil, err
	}
	return tree.Encode()
}

// Unmarshal decodes into a value tree and copies it back into a struct.
func (c *Codec) Unmarshal(wire []byte) (*matrix3.Scalars3, error) {
	tree, err := c.typ.Decode(wire)
	if err != nil {
		return nil, err
	}
	var out matrix3.Scalars3
	if err := c.typ.ExportModel(tree, &out, matrix3.Scalars3Meta); err != nil {
		return nil, err
	}
	return &out, nil
}
