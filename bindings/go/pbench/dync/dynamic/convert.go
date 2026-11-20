// Shipped model<->tree conversion for the C core: the same native struct the
// application holds moves into and out of a dynamic C tree through public API
// only. The pbench dync arm times exactly these functions.
//
// Export targets a fresh tree: absent model fields are never set, and unknown
// bytes have nowhere to come from on a struct. Required fields are not
// enforced.
package dynamic

import (
	"fmt"

	"pbwire/pbench/dyngo/codec"
)

// ImportModel copies a generated model into a new tree of this type.
func (t *MessageType) ImportModel(model any, meta *codec.Table) (*Message, error) {
	if model == nil {
		return nil, fmt.Errorf("dynamic: import a nil model")
	}
	if meta == nil {
		return nil, fmt.Errorf("dynamic: import without a model table")
	}
	m := t.New()
	if err := importInto(model, meta, m); err != nil {
		m.Close()
		return nil, err
	}
	return m, nil
}

// ExportModel copies a tree into a generated model.
func ExportModel(m *Message, model any, meta *codec.Table) error {
	if m == nil {
		return fmt.Errorf("dynamic: export a nil tree")
	}
	if model == nil {
		return fmt.Errorf("dynamic: export into a nil model")
	}
	if meta == nil {
		return fmt.Errorf("dynamic: export without a model table")
	}
	return exportInto(m, model, meta)
}

// importInto copies one message level from the model side to the tree side.
func importInto(model any, meta *codec.Table, m *Message) error {
	for _, row := range meta.Fields {
		if err := importRow(model, row, m); err != nil {
			return fmt.Errorf("dynamic: field %s: %w", row.Name, err)
		}
	}
	return nil
}

// importRow copies one field from the model row into the tree.
func importRow(model any, row *codec.Field, m *Message) error {
	if row.Repeated {
		return importRepeated(model, row, m)
	}
	if row.Oneof {
		v, ok := row.GetOneof(model)
		if !ok {
			return nil
		}
		return importOneofValue(v, row, m)
	}
	if row.Kind == codec.KindMessage {
		sub := row.GetMsg(model)
		if sub == nil {
			return nil
		}
		return importMessage(sub, row, m)
	}
	return importScalar(model, row, m)
}

// importRepeated copies one repeated field element by element.
func importRepeated(model any, row *codec.Field, m *Message) error {
	if row.Kind == codec.KindMessage {
		n := row.RepMsgLen(model)
		for k := 0; k < n; k++ {
			if err := importMessage(row.RepMsgAt(model, k), row, m); err != nil {
				return err
			}
		}
		return nil
	}
	return importRepScalar(model, row, m)
}

// importMessage copies one nested message, crossing a package boundary with
// the tree's own wire-bytes bridge when the model row is foreign.
func importMessage(sub any, row *codec.Field, m *Message) error {
	if row.Foreign != nil {
		raw, err := row.Foreign.Encode(sub)
		if err != nil {
			return err
		}
		return m.SetMessageBytes(row.Name, raw)
	}
	child, err := m.NewMessage(row.Name)
	if err != nil {
		return err
	}
	return importInto(sub, row.Sub(), child)
}

// importOneofValue converts a set oneof member for the tree side.
func importOneofValue(v any, row *codec.Field, m *Message) error {
	if row.Kind != codec.KindMessage {
		return importScalarValue(v, row, m)
	}
	return importMessage(v, row, m)
}

// exportInto copies one message level from the tree side to the model side.
func exportInto(m *Message, model any, meta *codec.Table) error {
	for _, row := range meta.Fields {
		if err := exportRow(m, row, model); err != nil {
			return fmt.Errorf("dynamic: field %s: %w", row.Name, err)
		}
	}
	return nil
}

// exportRow copies one field from the tree into the model row.
func exportRow(m *Message, row *codec.Field, model any) error {
	if row.Repeated {
		return exportRepeated(m, row, model)
	}
	if row.Oneof {
		return exportOneof(m, row, model)
	}
	if row.Kind == codec.KindMessage {
		sub, ok, err := m.GetMessage(row.Name)
		if err != nil {
			return err
		}
		if !ok {
			return nil
		}
		target := row.GetOrCreateMsg(model)
		if target == nil {
			return fmt.Errorf("dynamic: cannot create model submessage")
		}
		return exportMessage(sub, row, target)
	}
	return exportScalar(m, row, model)
}

// exportRepeated copies one repeated field element by element.
func exportRepeated(m *Message, row *codec.Field, model any) error {
	if row.Kind == codec.KindMessage {
		subs, err := m.GetMessageSlice(row.Name)
		if err != nil {
			return err
		}
		for _, sub := range subs {
			target := row.NewRepMsg(model)
			if target == nil {
				return fmt.Errorf("dynamic: cannot append model submessage")
			}
			if err := exportMessage(sub, row, target); err != nil {
				return err
			}
		}
		return nil
	}
	return exportRepScalar(m, row, model)
}

// exportMessage copies one nested message, crossing a package boundary with
// the tree's own wire-bytes bridge when the model row is foreign.
func exportMessage(sub *Message, row *codec.Field, target any) error {
	if row.Foreign != nil {
		wire, err := sub.Encode()
		if err != nil {
			return err
		}
		return row.Foreign.Decode(target, wire)
	}
	return exportInto(sub, target, row.Sub())
}

// exportOneof reads a set oneof member from the tree and wraps it for the
// model side. The last occurrence wins on decode already, so the tree holds
// at most one member and the wrapper choice is unambiguous.
func exportOneof(m *Message, row *codec.Field, model any) error {
	present, err := m.Has(row.Name)
	if err != nil {
		return err
	}
	if !present {
		return nil
	}
	if row.Kind != codec.KindMessage {
		v, err := readScalar(m, row)
		if err != nil {
			return err
		}
		row.SetOneof(model, v)
		return nil
	}
	sub, ok, err := m.GetMessage(row.Name)
	if err != nil {
		return err
	}
	if !ok {
		return nil
	}
	target := row.NewMsg()
	if target == nil {
		return fmt.Errorf("dynamic: cannot create model oneof member")
	}
	if err := exportMessage(sub, row, target); err != nil {
		return err
	}
	row.SetOneof(model, target)
	return nil
}
