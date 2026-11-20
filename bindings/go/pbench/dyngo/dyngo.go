// Package dyngo is the dynamic Go core arm: the struct is copied field by
// field into a codec DMsg value tree, which the shared table engine encodes
// with no cgo. Same user-visible task as the other arms, so the numbers
// compare like for like.
package dyngo

import (
	"fmt"
	"os"
	"path/filepath"
	"runtime"

	"pbwire/codec"
	"pbwire/tests/gen/matrix3"
)

// Codec owns the loaded schema and the resolved dynamic type.
type Codec struct {
	schema *codec.Schema
	typ    *codec.DType
}

func descriptorPath() string {
	_, file, _, _ := runtime.Caller(0)
	return filepath.Join(filepath.Dir(file), "..", "..", "codec", "testdata", "matrix3.desc")
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
	tree := c.typ.New()
	if err := fill(tree, m); err != nil {
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
	return load(tree)
}

// fill writes every field of the struct into the value tree.
func fill(t *codec.DMsg, m *matrix3.Scalars3) error {
	for _, err := range []error{
		t.SetInt("i32", int64(m.I32)),
		t.SetInt("i64", m.I64),
		t.SetUint("u32", uint64(m.U32)),
		t.SetUint("u64", m.U64),
		t.SetInt("s32", int64(m.S32)),
		t.SetInt("s64", m.S64),
		t.SetUint("fx32", uint64(m.Fx32)),
		t.SetUint("fx64", m.Fx64),
		t.SetInt("sfx32", int64(m.Sfx32)),
		t.SetInt("sfx64", m.Sfx64),
		t.SetFloat("f32", m.F32),
		t.SetDouble("f64", m.F64),
		t.SetBool("flag", m.Flag),
		t.SetString("str", m.Str),
		t.SetInt("color", int64(m.Color)),
	} {
		if err != nil {
			return err
		}
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
	for _, v := range m.Flags {
		if err := t.SetBool("flags", v); err != nil {
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
	return nil
}

// load reads every field of the tree back into a struct.
func load(t *codec.DMsg) (*matrix3.Scalars3, error) {
	m := &matrix3.Scalars3{}
	err := loadScalars(m, t)
	if err != nil {
		return nil, err
	}
	return m, nil
}

func loadScalars(m *matrix3.Scalars3, t *codec.DMsg) error {
	if v, _, err := t.GetInt("i32"); err != nil {
		return err
	} else {
		m.I32 = int32(v)
	}
	if v, _, err := t.GetInt("i64"); err != nil {
		return err
	} else {
		m.I64 = v
	}
	if v, _, err := t.GetUint("u32"); err != nil {
		return err
	} else {
		m.U32 = uint32(v)
	}
	if v, _, err := t.GetUint("u64"); err != nil {
		return err
	} else {
		m.U64 = v
	}
	if v, _, err := t.GetInt("s32"); err != nil {
		return err
	} else {
		m.S32 = int32(v)
	}
	if v, _, err := t.GetInt("s64"); err != nil {
		return err
	} else {
		m.S64 = v
	}
	if v, _, err := t.GetUint("fx32"); err != nil {
		return err
	} else {
		m.Fx32 = uint32(v)
	}
	if v, _, err := t.GetUint("fx64"); err != nil {
		return err
	} else {
		m.Fx64 = v
	}
	if v, _, err := t.GetInt("sfx32"); err != nil {
		return err
	} else {
		m.Sfx32 = int32(v)
	}
	if v, _, err := t.GetInt("sfx64"); err != nil {
		return err
	} else {
		m.Sfx64 = v
	}
	if v, _, err := t.GetFloat("f32"); err != nil {
		return err
	} else {
		m.F32 = v
	}
	if v, _, err := t.GetDouble("f64"); err != nil {
		return err
	} else {
		m.F64 = v
	}
	if v, _, err := t.GetBool("flag"); err != nil {
		return err
	} else {
		m.Flag = v
	}
	if v, _, err := t.GetString("str"); err != nil {
		return err
	} else {
		m.Str = v
	}
	if v, ok, err := t.GetInt("color"); err != nil {
		return err
	} else if ok {
		m.Color = matrix3.Color3(v)
	}
	if v, ok, err := t.GetInt("opt_i32"); err != nil {
		return err
	} else if ok {
		p := int32(v)
		m.OptI32 = &p
	}
	if v, ok, err := t.GetString("opt_str"); err != nil {
		return err
	} else if ok {
		m.OptStr = &v
	}
	if sub, ok, err := t.GetMessage("inner"); err != nil {
		return err
	} else if ok {
		inner, err := loadInner(sub)
		if err != nil {
			return err
		}
		m.Inner = inner
	}
	if nums, err := t.GetIntSlice("nums"); err != nil {
		return err
	} else {
		for _, v := range nums {
			m.Nums = append(m.Nums, int32(v))
		}
	}
	if tags, err := t.GetStringSlice("tags"); err != nil {
		return err
	} else {
		m.Tags = tags
	}
	if inners, err := t.GetMessageSlice("inners"); err != nil {
		return err
	} else {
		for _, sub := range inners {
			inner, err := loadInner(sub)
			if err != nil {
				return err
			}
			m.Inners = append(m.Inners, inner)
		}
	}
	if flags, err := t.GetBoolSlice("flags"); err != nil {
		return err
	} else {
		m.Flags = flags
	}
	return nil
}

func loadInner(t *codec.DMsg) (*matrix3.Inner3, error) {
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
