// Package dync is the dynamic C core arm: the struct is copied field by field
// into a pbwire C tree through cgo, encoded by the shared core, and decoded
// back through the same per-field crossings. It prices what a caller pays when
// the schema lives only in a FileDescriptorSet.
package dync

import (
	"fmt"
	"os"
	"path/filepath"
	"runtime"

	"pbwire/dynamic"
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
	return filepath.Join(filepath.Dir(file), "..", "..", "codec", "testdata", "matrix3.desc")
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
	tree := c.typ.New()
	defer tree.Close()
	if err := fill(tree, m); err != nil {
		return nil, err
	}
	return tree.Encode()
}

// Unmarshal decodes into a C tree and copies it back into a struct.
func (c *Codec) Unmarshal(wire []byte) (*matrix3.Scalars3, error) {
	tree, err := c.typ.Decode(wire)
	if err != nil {
		return nil, err
	}
	defer tree.Close()
	return load(tree)
}

// fill writes every field of the struct into the C tree.
func fill(t *dynamic.Message, m *matrix3.Scalars3) error {
	if err := t.SetInt("i32", int64(m.I32)); err != nil {
		return err
	}
	if err := t.SetInt("i64", m.I64); err != nil {
		return err
	}
	if err := t.SetUint("u32", uint64(m.U32)); err != nil {
		return err
	}
	if err := t.SetUint("u64", m.U64); err != nil {
		return err
	}
	if err := t.SetInt("s32", int64(m.S32)); err != nil {
		return err
	}
	if err := t.SetInt("s64", m.S64); err != nil {
		return err
	}
	if err := t.SetUint("fx32", uint64(m.Fx32)); err != nil {
		return err
	}
	if err := t.SetUint("fx64", m.Fx64); err != nil {
		return err
	}
	if err := t.SetInt("sfx32", int64(m.Sfx32)); err != nil {
		return err
	}
	if err := t.SetInt("sfx64", m.Sfx64); err != nil {
		return err
	}
	if err := t.SetFloat("f32", m.F32); err != nil {
		return err
	}
	if err := t.SetDouble("f64", m.F64); err != nil {
		return err
	}
	if err := t.SetBool("flag", m.Flag); err != nil {
		return err
	}
	if err := t.SetString("str", m.Str); err != nil {
		return err
	}
	if err := t.SetInt("color", int64(m.Color)); err != nil {
		return err
	}
	if m.OptI32 != nil {
		if err := t.SetInt("opt_i32", int64(*m.OptI32)); err != nil {
			return err
		}
	}
	if m.OptStr != nil {
		if err := t.SetString("opt_str", *m.OptStr); err != nil {
			return err
		}
	}
	if m.Inner != nil {
		sub, err := t.NewMessage("inner")
		if err != nil {
			return err
		}
		if err := sub.SetString("name", m.Inner.Name); err != nil {
			return err
		}
		if err := sub.SetInt("value", int64(m.Inner.Value)); err != nil {
			return err
		}
	}
	for _, v := range m.Nums {
		if err := t.SetInt("nums", int64(v)); err != nil {
			return err
		}
	}
	for _, v := range m.Tags {
		if err := t.SetString("tags", v); err != nil {
			return err
		}
	}
	for _, v := range m.Inners {
		sub, err := t.NewMessage("inners")
		if err != nil {
			return err
		}
		if err := sub.SetString("name", v.Name); err != nil {
			return err
		}
		if err := sub.SetInt("value", int64(v.Value)); err != nil {
			return err
		}
	}
	for _, v := range m.Flags {
		if err := t.SetBool("flags", v); err != nil {
			return err
		}
	}
	return nil
}

// load reads every field of the tree back into a struct.
func load(t *dynamic.Message) (*matrix3.Scalars3, error) {
	m := &matrix3.Scalars3{}
	v, _, err := t.GetInt("i32")
	if err != nil {
		return nil, err
	}
	m.I32 = int32(v)
	if m.I64, _, err = t.GetInt("i64"); err != nil {
		return nil, err
	}
	if v, _, err := t.GetUint("u32"); err != nil {
		return nil, err
	} else {
		m.U32 = uint32(v)
	}
	if m.U64, _, err = t.GetUint("u64"); err != nil {
		return nil, err
	}
	if v, _, err := t.GetInt("s32"); err != nil {
		return nil, err
	} else {
		m.S32 = int32(v)
	}
	if m.S64, _, err = t.GetInt("s64"); err != nil {
		return nil, err
	}
	if v, _, err := t.GetUint("fx32"); err != nil {
		return nil, err
	} else {
		m.Fx32 = uint32(v)
	}
	if m.Fx64, _, err = t.GetUint("fx64"); err != nil {
		return nil, err
	}
	if v, _, err := t.GetInt("sfx32"); err != nil {
		return nil, err
	} else {
		m.Sfx32 = int32(v)
	}
	if m.Sfx64, _, err = t.GetInt("sfx64"); err != nil {
		return nil, err
	}
	if m.F32, _, err = t.GetFloat("f32"); err != nil {
		return nil, err
	}
	if m.F64, _, err = t.GetDouble("f64"); err != nil {
		return nil, err
	}
	if m.Flag, _, err = t.GetBool("flag"); err != nil {
		return nil, err
	}
	if m.Str, _, err = t.GetString("str"); err != nil {
		return nil, err
	}
	if v, ok, err := t.GetInt("color"); err != nil {
		return nil, err
	} else if ok {
		m.Color = matrix3.Color3(v)
	}
	if v, ok, err := t.GetInt("opt_i32"); err != nil {
		return nil, err
	} else if ok {
		p := int32(v)
		m.OptI32 = &p
	}
	if v, ok, err := t.GetString("opt_str"); err != nil {
		return nil, err
	} else if ok {
		m.OptStr = &v
	}
	if sub, ok, err := t.GetMessage("inner"); err != nil {
		return nil, err
	} else if ok {
		inner, err := loadInner(sub)
		if err != nil {
			return nil, err
		}
		m.Inner = inner
	}
	if nums, err := t.GetIntSlice("nums"); err != nil {
		return nil, err
	} else {
		for _, v := range nums {
			m.Nums = append(m.Nums, int32(v))
		}
	}
	if tags, err := t.GetStringSlice("tags"); err != nil {
		return nil, err
	} else {
		m.Tags = tags
	}
	if inners, err := t.GetMessageSlice("inners"); err != nil {
		return nil, err
	} else {
		for _, sub := range inners {
			inner, err := loadInner(sub)
			if err != nil {
				return nil, err
			}
			m.Inners = append(m.Inners, inner)
		}
	}
	if flags, err := t.GetBoolSlice("flags"); err != nil {
		return nil, err
	} else {
		m.Flags = flags
	}
	return m, nil
}

func loadInner(t *dynamic.Message) (*matrix3.Inner3, error) {
	inner := &matrix3.Inner3{}
	if name, _, err := t.GetString("name"); err != nil {
		return nil, err
	} else {
		inner.Name = name
	}
	if v, _, err := t.GetInt("value"); err != nil {
		return nil, err
	} else {
		inner.Value = int32(v)
	}
	return inner, nil
}
