// Shipped model<->tree conversion. The application holds a generated thin
// model; the dynamic cores hold a value tree. These functions move a message
// between the two through the tables both sides already share, with no
// reflection and no wire round trip: every row copies getter to setter,
// nested messages recurse, unknown fields stay on the tree.
//
// Export targets a fresh zero struct: absent tree fields leave the struct
// field at its zero value, and unknown bytes have nowhere to go on a struct
// and are dropped (documented, matching the thin model's decode contract).
// Required fields are not enforced on either direction.
package runtime

import (
	"fmt"
)

// ImportModel copies a generated model into a new tree of this type.
func (t *DType) ImportModel(model any, meta *Table) (*DMsg, error) {
	if model == nil {
		return nil, fmt.Errorf("codec: import a nil model")
	}
	if meta == nil {
		return nil, fmt.Errorf("codec: import without a model table")
	}
	d := t.New()
	if err := importInto(model, meta, d, t); err != nil {
		return nil, err
	}
	return d, nil
}

// ExportModel copies a tree into a generated model.
func (t *DType) ExportModel(d *DMsg, model any, meta *Table) error {
	if d == nil {
		return fmt.Errorf("codec: export a nil tree")
	}
	if model == nil {
		return fmt.Errorf("codec: export into a nil model")
	}
	if meta == nil {
		return fmt.Errorf("codec: export without a model table")
	}
	return exportInto(d, model, meta, t)
}

// importInto copies one message level from the model side to the tree side.
func importInto(model any, meta *Table, d *DMsg, t *DType) error {
	rt := t.tableFor()
	for i, fp := range t.plan.Fields {
		mrow := lookup(meta, fp.Number)
		if mrow == nil {
			return fmt.Errorf("codec: model table %s has no field %d", meta.Name, fp.Number)
		}
		drow := lookup(rt, fp.Number)
		if drow == nil {
			return fmt.Errorf("codec: type %s has no field %d", t.plan.Full, fp.Number)
		}
		if err := importRow(t, i, model, mrow, d, drow); err != nil {
			return fmt.Errorf("codec: field %s: %w", fp.Name, err)
		}
	}
	return nil
}

// importRow copies one field from the model row to the tree row.
func importRow(t *DType, i int, model any, mrow *Field, d *DMsg, drow *Field) error {
	if mrow.Repeated {
		return importRepeated(t, i, model, mrow, d, drow)
	}
	if mrow.Oneof {
		v, ok := mrow.GetOneof(model)
		if !ok {
			return nil
		}
		cv, err := importOneofValue(t, i, v, mrow)
		if err != nil {
			return err
		}
		drow.SetOneof(d, cv)
		return nil
	}
	if mrow.Kind == KindMessage {
		sub := mrow.GetMsg(model)
		if sub == nil {
			return nil
		}
		child, err := importMessage(t, i, sub, mrow)
		if err != nil {
			return err
		}
		s := &d.slots[i]
		s.m, s.present = child, true
		return nil
	}
	return importScalar(model, mrow, d, drow)
}

// importRepeated copies one repeated field element by element.
func importRepeated(t *DType, i int, model any, mrow *Field, d *DMsg, drow *Field) error {
	if mrow.Kind == KindMessage {
		n := mrow.RepMsgLen(model)
		for k := 0; k < n; k++ {
			sub := mrow.RepMsgAt(model, k)
			child, err := importMessage(t, i, sub, mrow)
			if err != nil {
				return err
			}
			s := &d.slots[i]
			s.present = true
			appendRep(s, child)
		}
		return nil
	}
	return importRepScalar(model, mrow, d, drow)
}

// importMessage copies one nested message, crossing a package boundary
// through the sibling's own wire form when the model row is foreign.
func importMessage(t *DType, i int, sub any, mrow *Field) (*DMsg, error) {
	st, err := t.sub(i)
	if err != nil {
		return nil, err
	}
	if mrow.Foreign != nil {
		raw, err := mrow.Foreign.Encode(sub)
		if err != nil {
			return nil, err
		}
		return st.Decode(raw)
	}
	if sub == nil {
		return st.New(), nil
	}
	child := st.New()
	if err := importInto(sub, mrow.Sub(), child, st); err != nil {
		return nil, err
	}
	return child, nil
}

// importOneofValue converts a set oneof member for the tree side. Message
// members arrive as generated structs and leave as value trees; scalars pass
// through unwrapped.
func importOneofValue(t *DType, i int, v any, mrow *Field) (any, error) {
	if mrow.Kind != KindMessage {
		return v, nil
	}
	return importMessage(t, i, v, mrow)
}

// exportInto copies one message level from the tree side to the model side.
func exportInto(d *DMsg, model any, meta *Table, t *DType) error {
	rt := t.tableFor()
	for i, fp := range t.plan.Fields {
		mrow := lookup(meta, fp.Number)
		if mrow == nil {
			return fmt.Errorf("codec: model table %s has no field %d", meta.Name, fp.Number)
		}
		drow := lookup(rt, fp.Number)
		if drow == nil {
			return fmt.Errorf("codec: type %s has no field %d", t.plan.Full, fp.Number)
		}
		if err := exportRow(t, i, d, drow, model, mrow); err != nil {
			return fmt.Errorf("codec: field %s: %w", fp.Name, err)
		}
	}
	return nil
}

// exportRow copies one field from the tree row to the model row.
func exportRow(t *DType, i int, d *DMsg, drow *Field, model any, mrow *Field) error {
	if mrow.Repeated {
		return exportRepeated(t, i, d, drow, model, mrow)
	}
	if mrow.Oneof {
		v, ok := drow.GetOneof(d)
		if !ok {
			return nil
		}
		cv, err := exportOneofValue(t, i, v, mrow)
		if err != nil {
			return err
		}
		mrow.SetOneof(model, cv)
		return nil
	}
	if mrow.Kind == KindMessage {
		sub := drow.GetMsg(d)
		if sub == nil {
			return nil
		}
		target := mrow.GetOrCreateMsg(model)
		if target == nil {
			return fmt.Errorf("codec: cannot create model submessage")
		}
		return exportMessage(t, i, sub.(*DMsg), target, mrow)
	}
	return exportScalar(d, drow, model, mrow)
}

// exportRepeated copies one repeated field element by element.
func exportRepeated(t *DType, i int, d *DMsg, drow *Field, model any, mrow *Field) error {
	if mrow.Kind == KindMessage {
		n := drow.RepMsgLen(d)
		for k := 0; k < n; k++ {
			sub := drow.RepMsgAt(d, k)
			target := mrow.NewRepMsg(model)
			if target == nil {
				return fmt.Errorf("codec: cannot append model submessage")
			}
			if err := exportMessage(t, i, sub.(*DMsg), target, mrow); err != nil {
				return err
			}
		}
		return nil
	}
	return exportRepScalar(d, drow, model, mrow)
}

// exportMessage copies one nested message, crossing a package boundary
// through the sibling's own wire form when the model row is foreign.
func exportMessage(t *DType, i int, sub *DMsg, target any, mrow *Field) error {
	st, err := t.sub(i)
	if err != nil {
		return err
	}
	if mrow.Foreign != nil {
		raw, err := sub.Encode()
		if err != nil {
			return err
		}
		return mrow.Foreign.Decode(target, raw)
	}
	return exportInto(sub, target, mrow.Sub(), st)
}

// exportOneofValue converts a set oneof member for the model side.
func exportOneofValue(t *DType, i int, v any, mrow *Field) (any, error) {
	if mrow.Kind != KindMessage {
		return v, nil
	}
	sub, ok := v.(*DMsg)
	if !ok {
		return nil, fmt.Errorf("codec: oneof member is not a value tree")
	}
	target := mrow.NewMsg()
	if target == nil {
		return nil, fmt.Errorf("codec: cannot create model oneof member")
	}
	st, err := t.sub(i)
	if err != nil {
		return nil, err
	}
	if mrow.Foreign != nil {
		raw, err := sub.Encode()
		if err != nil {
			return nil, err
		}
		if err := mrow.Foreign.Decode(target, raw); err != nil {
			return nil, err
		}
		return target, nil
	}
	if err := exportInto(sub, target, mrow.Sub(), st); err != nil {
		return nil, err
	}
	return target, nil
}
