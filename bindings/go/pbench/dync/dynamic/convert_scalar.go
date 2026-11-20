// Scalar legs of model<->tree conversion for the C core. Kind maps onto the
// dynamic API's integer-family setters; presence mirrors the engine: implicit
// zeros are never written, explicit pointers only when non-nil.

package dynamic

import (
	"fmt"

	"pbwire/pbench/dyngo/codec"
)

// importScalar copies one singular scalar, string, bytes or enum field.
func importScalar(model any, row *codec.Field, m *Message) error {
	switch row.Kind {
	case codec.KindInt32:
		if row.Explicit {
			if p := row.GetOptI32(model); p != nil {
				return m.SetInt(row.Name, int64(*p))
			}
			return nil
		}
		if v := row.GetI32(model); v != 0 || row.Required {
			return m.SetInt(row.Name, int64(v))
		}
	case codec.KindInt64:
		if row.Explicit {
			if p := row.GetOptI64(model); p != nil {
				return m.SetInt(row.Name, *p)
			}
			return nil
		}
		if v := row.GetI64(model); v != 0 || row.Required {
			return m.SetInt(row.Name, v)
		}
	case codec.KindUint32:
		if row.Explicit {
			if p := row.GetOptU32(model); p != nil {
				return m.SetUint(row.Name, uint64(*p))
			}
			return nil
		}
		if v := row.GetU32(model); v != 0 || row.Required {
			return m.SetUint(row.Name, uint64(v))
		}
	case codec.KindUint64:
		if row.Explicit {
			if p := row.GetOptU64(model); p != nil {
				return m.SetUint(row.Name, *p)
			}
			return nil
		}
		if v := row.GetU64(model); v != 0 || row.Required {
			return m.SetUint(row.Name, v)
		}
	case codec.KindSint32:
		if row.Explicit {
			if p := row.GetOptS32(model); p != nil {
				return m.SetInt(row.Name, int64(*p))
			}
			return nil
		}
		if v := row.GetS32(model); v != 0 || row.Required {
			return m.SetInt(row.Name, int64(v))
		}
	case codec.KindSint64:
		if row.Explicit {
			if p := row.GetOptS64(model); p != nil {
				return m.SetInt(row.Name, *p)
			}
			return nil
		}
		if v := row.GetS64(model); v != 0 || row.Required {
			return m.SetInt(row.Name, v)
		}
	case codec.KindFixed32:
		if row.Explicit {
			if p := row.GetOptFx32(model); p != nil {
				return m.SetUint(row.Name, uint64(*p))
			}
			return nil
		}
		if v := row.GetFx32(model); v != 0 || row.Required {
			return m.SetUint(row.Name, uint64(v))
		}
	case codec.KindFixed64:
		if row.Explicit {
			if p := row.GetOptFx64(model); p != nil {
				return m.SetUint(row.Name, *p)
			}
			return nil
		}
		if v := row.GetFx64(model); v != 0 || row.Required {
			return m.SetUint(row.Name, v)
		}
	case codec.KindSfixed32:
		if row.Explicit {
			if p := row.GetOptSfx32(model); p != nil {
				return m.SetInt(row.Name, int64(*p))
			}
			return nil
		}
		if v := row.GetSfx32(model); v != 0 || row.Required {
			return m.SetInt(row.Name, int64(v))
		}
	case codec.KindSfixed64:
		if row.Explicit {
			if p := row.GetOptSfx64(model); p != nil {
				return m.SetInt(row.Name, *p)
			}
			return nil
		}
		if v := row.GetSfx64(model); v != 0 || row.Required {
			return m.SetInt(row.Name, v)
		}
	case codec.KindFloat:
		if row.Explicit {
			if p := row.GetOptF32(model); p != nil {
				return m.SetFloat(row.Name, *p)
			}
			return nil
		}
		if v := row.GetF32(model); v != 0 || row.Required {
			return m.SetFloat(row.Name, v)
		}
	case codec.KindDouble:
		if row.Explicit {
			if p := row.GetOptF64(model); p != nil {
				return m.SetDouble(row.Name, *p)
			}
			return nil
		}
		if v := row.GetF64(model); v != 0 || row.Required {
			return m.SetDouble(row.Name, v)
		}
	case codec.KindBool:
		if row.Explicit {
			if p := row.GetOptBool(model); p != nil {
				return m.SetBool(row.Name, *p)
			}
			return nil
		}
		if v := row.GetBool(model); v || row.Required {
			return m.SetBool(row.Name, v)
		}
	case codec.KindString:
		if row.Explicit {
			if p := row.GetOptStr(model); p != nil {
				return m.SetString(row.Name, *p)
			}
			return nil
		}
		if v := row.GetStr(model); v != "" || row.Required {
			return m.SetString(row.Name, v)
		}
	case codec.KindBytes:
		if row.Explicit {
			if p := row.GetOptBytes(model); p != nil {
				return m.SetBytes(row.Name, *p)
			}
			return nil
		}
		if v := row.GetBytes(model); len(v) != 0 || row.Required {
			return m.SetBytes(row.Name, v)
		}
	case codec.KindEnum:
		if row.Explicit {
			if p := row.GetOptEnum(model); p != nil {
				return m.SetInt(row.Name, int64(*p))
			}
			return nil
		}
		if v := row.GetEnum(model); v != 0 || row.Required {
			return m.SetInt(row.Name, int64(v))
		}
	}
	return nil
}

// importScalarValue stores a set oneof scalar member, already unwrapped.
func importScalarValue(v any, row *codec.Field, m *Message) error {
	switch t := v.(type) {
	case int32:
		return m.SetInt(row.Name, int64(t))
	case int64:
		return m.SetInt(row.Name, t)
	case uint32:
		return m.SetUint(row.Name, uint64(t))
	case uint64:
		return m.SetUint(row.Name, t)
	case float32:
		return m.SetFloat(row.Name, t)
	case float64:
		return m.SetDouble(row.Name, t)
	case bool:
		return m.SetBool(row.Name, t)
	case string:
		return m.SetString(row.Name, t)
	case []byte:
		return m.SetBytes(row.Name, t)
	}
	return fmt.Errorf("dynamic: oneof member has no scalar form")
}

// importRepScalar copies one repeated scalar run element by element.
func importRepScalar(model any, row *codec.Field, m *Message) error {
	switch row.Kind {
	case codec.KindInt32:
		for _, v := range row.GetRepI32(model) {
			if err := m.SetInt(row.Name, int64(v)); err != nil {
				return err
			}
		}
	case codec.KindInt64:
		for _, v := range row.GetRepI64(model) {
			if err := m.SetInt(row.Name, v); err != nil {
				return err
			}
		}
	case codec.KindUint32:
		for _, v := range row.GetRepU32(model) {
			if err := m.SetUint(row.Name, uint64(v)); err != nil {
				return err
			}
		}
	case codec.KindUint64:
		for _, v := range row.GetRepU64(model) {
			if err := m.SetUint(row.Name, v); err != nil {
				return err
			}
		}
	case codec.KindSint32:
		for _, v := range row.GetRepS32(model) {
			if err := m.SetInt(row.Name, int64(v)); err != nil {
				return err
			}
		}
	case codec.KindSint64:
		for _, v := range row.GetRepS64(model) {
			if err := m.SetInt(row.Name, v); err != nil {
				return err
			}
		}
	case codec.KindFixed32:
		for _, v := range row.GetRepFx32(model) {
			if err := m.SetUint(row.Name, uint64(v)); err != nil {
				return err
			}
		}
	case codec.KindFixed64:
		for _, v := range row.GetRepFx64(model) {
			if err := m.SetUint(row.Name, v); err != nil {
				return err
			}
		}
	case codec.KindSfixed32:
		for _, v := range row.GetRepSfx32(model) {
			if err := m.SetInt(row.Name, int64(v)); err != nil {
				return err
			}
		}
	case codec.KindSfixed64:
		for _, v := range row.GetRepSfx64(model) {
			if err := m.SetInt(row.Name, v); err != nil {
				return err
			}
		}
	case codec.KindFloat:
		for _, v := range row.GetRepF32(model) {
			if err := m.SetFloat(row.Name, v); err != nil {
				return err
			}
		}
	case codec.KindDouble:
		for _, v := range row.GetRepF64(model) {
			if err := m.SetDouble(row.Name, v); err != nil {
				return err
			}
		}
	case codec.KindBool:
		for _, v := range row.GetRepBool(model) {
			if err := m.SetBool(row.Name, v); err != nil {
				return err
			}
		}
	case codec.KindString:
		for _, v := range row.GetRepStr(model) {
			if err := m.SetString(row.Name, v); err != nil {
				return err
			}
		}
	case codec.KindBytes:
		for _, v := range row.GetRepBytes(model) {
			if err := m.SetBytes(row.Name, v); err != nil {
				return err
			}
		}
	case codec.KindEnum:
		for _, v := range row.GetRepEnum(model) {
			if err := m.SetInt(row.Name, int64(v)); err != nil {
				return err
			}
		}
	}
	return nil
}

// exportScalar copies one singular field from the tree to the model. Absent
// tree fields leave the model field at its zero value.
func exportScalar(m *Message, row *codec.Field, model any) error {
	switch row.Kind {
	case codec.KindInt32:
		v, ok, err := m.GetInt(row.Name)
		if err != nil || !ok {
			return err
		}
		if row.Explicit {
			row.SetOptI32(model, int32(v))
		} else {
			row.SetI32(model, int32(v))
		}
	case codec.KindInt64:
		v, ok, err := m.GetInt(row.Name)
		if err != nil || !ok {
			return err
		}
		if row.Explicit {
			row.SetOptI64(model, v)
		} else {
			row.SetI64(model, v)
		}
	case codec.KindUint32:
		v, ok, err := m.GetUint(row.Name)
		if err != nil || !ok {
			return err
		}
		if row.Explicit {
			row.SetOptU32(model, uint32(v))
		} else {
			row.SetU32(model, uint32(v))
		}
	case codec.KindUint64:
		v, ok, err := m.GetUint(row.Name)
		if err != nil || !ok {
			return err
		}
		if row.Explicit {
			row.SetOptU64(model, v)
		} else {
			row.SetU64(model, v)
		}
	case codec.KindSint32:
		v, ok, err := m.GetInt(row.Name)
		if err != nil || !ok {
			return err
		}
		if row.Explicit {
			row.SetOptS32(model, int32(v))
		} else {
			row.SetS32(model, int32(v))
		}
	case codec.KindSint64:
		v, ok, err := m.GetInt(row.Name)
		if err != nil || !ok {
			return err
		}
		if row.Explicit {
			row.SetOptS64(model, v)
		} else {
			row.SetS64(model, v)
		}
	case codec.KindFixed32:
		v, ok, err := m.GetUint(row.Name)
		if err != nil || !ok {
			return err
		}
		if row.Explicit {
			row.SetOptFx32(model, uint32(v))
		} else {
			row.SetFx32(model, uint32(v))
		}
	case codec.KindFixed64:
		v, ok, err := m.GetUint(row.Name)
		if err != nil || !ok {
			return err
		}
		if row.Explicit {
			row.SetOptFx64(model, v)
		} else {
			row.SetFx64(model, v)
		}
	case codec.KindSfixed32:
		v, ok, err := m.GetInt(row.Name)
		if err != nil || !ok {
			return err
		}
		if row.Explicit {
			row.SetOptSfx32(model, int32(v))
		} else {
			row.SetSfx32(model, int32(v))
		}
	case codec.KindSfixed64:
		v, ok, err := m.GetInt(row.Name)
		if err != nil || !ok {
			return err
		}
		if row.Explicit {
			row.SetOptSfx64(model, v)
		} else {
			row.SetSfx64(model, v)
		}
	case codec.KindFloat:
		v, ok, err := m.GetFloat(row.Name)
		if err != nil || !ok {
			return err
		}
		if row.Explicit {
			row.SetOptF32(model, v)
		} else {
			row.SetF32(model, v)
		}
	case codec.KindDouble:
		v, ok, err := m.GetDouble(row.Name)
		if err != nil || !ok {
			return err
		}
		if row.Explicit {
			row.SetOptF64(model, v)
		} else {
			row.SetF64(model, v)
		}
	case codec.KindBool:
		v, ok, err := m.GetBool(row.Name)
		if err != nil || !ok {
			return err
		}
		if row.Explicit {
			row.SetOptBool(model, v)
		} else {
			row.SetBool(model, v)
		}
	case codec.KindString:
		v, ok, err := m.GetString(row.Name)
		if err != nil || !ok {
			return err
		}
		if row.Explicit {
			row.SetOptStr(model, v)
		} else {
			row.SetStr(model, v)
		}
	case codec.KindBytes:
		v, ok, err := m.GetBytes(row.Name)
		if err != nil || !ok {
			return err
		}
		if row.Explicit {
			row.SetOptBytes(model, v)
		} else {
			row.SetBytes(model, v)
		}
	case codec.KindEnum:
		v, ok, err := m.GetInt(row.Name)
		if err != nil || !ok {
			return err
		}
		if row.Explicit {
			row.SetOptEnum(model, int32(v))
		} else {
			row.SetEnum(model, int32(v))
		}
	}
	return nil
}

// exportRepScalar copies one repeated scalar run element by element.
func exportRepScalar(m *Message, row *codec.Field, model any) error {
	switch row.Kind {
	case codec.KindInt32:
		vs, err := m.GetIntSlice(row.Name)
		if err != nil {
			return err
		}
		for _, v := range vs {
			row.AppendI32(model, int32(v))
		}
	case codec.KindInt64:
		vs, err := m.GetIntSlice(row.Name)
		if err != nil {
			return err
		}
		for _, v := range vs {
			row.AppendI64(model, v)
		}
	case codec.KindUint32:
		vs, err := m.GetUintSlice(row.Name)
		if err != nil {
			return err
		}
		for _, v := range vs {
			row.AppendU32(model, uint32(v))
		}
	case codec.KindUint64:
		vs, err := m.GetUintSlice(row.Name)
		if err != nil {
			return err
		}
		for _, v := range vs {
			row.AppendU64(model, v)
		}
	case codec.KindSint32:
		vs, err := m.GetIntSlice(row.Name)
		if err != nil {
			return err
		}
		for _, v := range vs {
			row.AppendS32(model, int32(v))
		}
	case codec.KindSint64:
		vs, err := m.GetIntSlice(row.Name)
		if err != nil {
			return err
		}
		for _, v := range vs {
			row.AppendS64(model, v)
		}
	case codec.KindFixed32:
		vs, err := m.GetUintSlice(row.Name)
		if err != nil {
			return err
		}
		for _, v := range vs {
			row.AppendFx32(model, uint32(v))
		}
	case codec.KindFixed64:
		vs, err := m.GetUintSlice(row.Name)
		if err != nil {
			return err
		}
		for _, v := range vs {
			row.AppendFx64(model, v)
		}
	case codec.KindSfixed32:
		vs, err := m.GetIntSlice(row.Name)
		if err != nil {
			return err
		}
		for _, v := range vs {
			row.AppendSfx32(model, int32(v))
		}
	case codec.KindSfixed64:
		vs, err := m.GetIntSlice(row.Name)
		if err != nil {
			return err
		}
		for _, v := range vs {
			row.AppendSfx64(model, v)
		}
	case codec.KindFloat:
		vs, err := m.GetFloatSlice(row.Name)
		if err != nil {
			return err
		}
		for _, v := range vs {
			row.AppendF32(model, v)
		}
	case codec.KindDouble:
		vs, err := m.GetDoubleSlice(row.Name)
		if err != nil {
			return err
		}
		for _, v := range vs {
			row.AppendF64(model, v)
		}
	case codec.KindBool:
		vs, err := m.GetBoolSlice(row.Name)
		if err != nil {
			return err
		}
		for _, v := range vs {
			row.AppendBool(model, v)
		}
	case codec.KindString:
		vs, err := m.GetStringSlice(row.Name)
		if err != nil {
			return err
		}
		for _, v := range vs {
			row.AppendStr(model, v)
		}
	case codec.KindBytes:
		vs, err := m.GetBytesSlice(row.Name)
		if err != nil {
			return err
		}
		for _, v := range vs {
			row.AppendBytes(model, v)
		}
	case codec.KindEnum:
		vs, err := m.GetIntSlice(row.Name)
		if err != nil {
			return err
		}
		for _, v := range vs {
			row.AppendEnum(model, int32(v))
		}
	}
	return nil
}

// readScalar reads a set oneof scalar member back in the wrapper's exact type.
func readScalar(m *Message, row *codec.Field) (any, error) {
	switch row.Kind {
	case codec.KindInt32, codec.KindSint32, codec.KindSfixed32, codec.KindEnum:
		v, _, err := m.GetInt(row.Name)
		return int32(v), err
	case codec.KindInt64, codec.KindSint64, codec.KindSfixed64:
		v, _, err := m.GetInt(row.Name)
		return v, err
	case codec.KindUint32, codec.KindFixed32:
		v, _, err := m.GetUint(row.Name)
		return uint32(v), err
	case codec.KindUint64, codec.KindFixed64:
		v, _, err := m.GetUint(row.Name)
		return v, err
	case codec.KindFloat:
		v, _, err := m.GetFloat(row.Name)
		return v, err
	case codec.KindDouble:
		v, _, err := m.GetDouble(row.Name)
		return v, err
	case codec.KindBool:
		v, _, err := m.GetBool(row.Name)
		return v, err
	case codec.KindString:
		v, _, err := m.GetString(row.Name)
		return v, err
	case codec.KindBytes:
		v, _, err := m.GetBytes(row.Name)
		return v, err
	}
	return nil, fmt.Errorf("dynamic: oneof member has no scalar form")
}
