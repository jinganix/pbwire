// Scalar legs of model<->tree conversion. Each leg moves one value from the
// model row's getter to the tree row's setter or back; presence follows the
// same rules the engine uses. On import, an implicit scalar carrying the zero
// value is not written at all, so Has on the tree agrees with a wire decode
// (where the field never occurred). Explicit (pointer) scalars write only
// when the pointer is non-nil.

package runtime

import "math"

// importScalar copies one singular scalar, string, bytes or enum field.
func importScalar(model any, mrow *Field, d *DMsg, drow *Field) error {
	switch mrow.Kind {
	case KindInt32:
		if mrow.Explicit {
			if p := mrow.GetOptI32(model); p != nil {
				drow.SetOptI32(d, *p)
			}
			return nil
		}
		if v := mrow.GetI32(model); v != 0 || mrow.Required {
			drow.SetI32(d, v)
		}
	case KindInt64:
		if mrow.Explicit {
			if p := mrow.GetOptI64(model); p != nil {
				drow.SetOptI64(d, *p)
			}
			return nil
		}
		if v := mrow.GetI64(model); v != 0 || mrow.Required {
			drow.SetI64(d, v)
		}
	case KindUint32:
		if mrow.Explicit {
			if p := mrow.GetOptU32(model); p != nil {
				drow.SetOptU32(d, *p)
			}
			return nil
		}
		if v := mrow.GetU32(model); v != 0 || mrow.Required {
			drow.SetU32(d, v)
		}
	case KindUint64:
		if mrow.Explicit {
			if p := mrow.GetOptU64(model); p != nil {
				drow.SetOptU64(d, *p)
			}
			return nil
		}
		if v := mrow.GetU64(model); v != 0 || mrow.Required {
			drow.SetU64(d, v)
		}
	case KindSint32:
		if mrow.Explicit {
			if p := mrow.GetOptS32(model); p != nil {
				drow.SetOptS32(d, *p)
			}
			return nil
		}
		if v := mrow.GetS32(model); v != 0 || mrow.Required {
			drow.SetS32(d, v)
		}
	case KindSint64:
		if mrow.Explicit {
			if p := mrow.GetOptS64(model); p != nil {
				drow.SetOptS64(d, *p)
			}
			return nil
		}
		if v := mrow.GetS64(model); v != 0 || mrow.Required {
			drow.SetS64(d, v)
		}
	case KindFixed32:
		if mrow.Explicit {
			if p := mrow.GetOptFx32(model); p != nil {
				drow.SetOptFx32(d, *p)
			}
			return nil
		}
		if v := mrow.GetFx32(model); v != 0 || mrow.Required {
			drow.SetFx32(d, v)
		}
	case KindFixed64:
		if mrow.Explicit {
			if p := mrow.GetOptFx64(model); p != nil {
				drow.SetOptFx64(d, *p)
			}
			return nil
		}
		if v := mrow.GetFx64(model); v != 0 || mrow.Required {
			drow.SetFx64(d, v)
		}
	case KindSfixed32:
		if mrow.Explicit {
			if p := mrow.GetOptSfx32(model); p != nil {
				drow.SetOptSfx32(d, *p)
			}
			return nil
		}
		if v := mrow.GetSfx32(model); v != 0 || mrow.Required {
			drow.SetSfx32(d, v)
		}
	case KindSfixed64:
		if mrow.Explicit {
			if p := mrow.GetOptSfx64(model); p != nil {
				drow.SetOptSfx64(d, *p)
			}
			return nil
		}
		if v := mrow.GetSfx64(model); v != 0 || mrow.Required {
			drow.SetSfx64(d, v)
		}
	case KindFloat:
		if mrow.Explicit {
			if p := mrow.GetOptF32(model); p != nil {
				drow.SetOptF32(d, *p)
			}
			return nil
		}
		if v := mrow.GetF32(model); math.Float32bits(v) != 0 || mrow.Required {
			drow.SetF32(d, v)
		}
	case KindDouble:
		if mrow.Explicit {
			if p := mrow.GetOptF64(model); p != nil {
				drow.SetOptF64(d, *p)
			}
			return nil
		}
		if v := mrow.GetF64(model); math.Float64bits(v) != 0 || mrow.Required {
			drow.SetF64(d, v)
		}
	case KindBool:
		if mrow.Explicit {
			if p := mrow.GetOptBool(model); p != nil {
				drow.SetOptBool(d, *p)
			}
			return nil
		}
		if v := mrow.GetBool(model); v || mrow.Required {
			drow.SetBool(d, v)
		}
	case KindString:
		if mrow.Explicit {
			if p := mrow.GetOptStr(model); p != nil {
				drow.SetOptStr(d, *p)
			}
			return nil
		}
		if v := mrow.GetStr(model); v != "" || mrow.Required {
			drow.SetStr(d, v)
		}
	case KindBytes:
		if mrow.Explicit {
			if p := mrow.GetOptBytes(model); p != nil {
				drow.SetOptBytes(d, *p)
			}
			return nil
		}
		if v := mrow.GetBytes(model); len(v) != 0 || mrow.Required {
			drow.SetBytes(d, v)
		}
	case KindEnum:
		if mrow.Explicit {
			if p := mrow.GetOptEnum(model); p != nil {
				drow.SetOptEnum(d, *p)
			}
			return nil
		}
		if v := mrow.GetEnum(model); v != 0 || mrow.Required {
			drow.SetEnum(d, v)
		}
	}
	return nil
}

// exportScalar copies one singular field from the tree to the model. Absent
// tree fields leave the model field at its zero value, which is why export
// targets a fresh struct.
func exportScalar(d *DMsg, drow *Field, model any, mrow *Field) error {
	switch mrow.Kind {
	case KindInt32:
		if mrow.Explicit {
			if p := drow.GetOptI32(d); p != nil {
				mrow.SetOptI32(model, *p)
			}
			return nil
		}
		mrow.SetI32(model, drow.GetI32(d))
	case KindInt64:
		if mrow.Explicit {
			if p := drow.GetOptI64(d); p != nil {
				mrow.SetOptI64(model, *p)
			}
			return nil
		}
		mrow.SetI64(model, drow.GetI64(d))
	case KindUint32:
		if mrow.Explicit {
			if p := drow.GetOptU32(d); p != nil {
				mrow.SetOptU32(model, *p)
			}
			return nil
		}
		mrow.SetU32(model, drow.GetU32(d))
	case KindUint64:
		if mrow.Explicit {
			if p := drow.GetOptU64(d); p != nil {
				mrow.SetOptU64(model, *p)
			}
			return nil
		}
		mrow.SetU64(model, drow.GetU64(d))
	case KindSint32:
		if mrow.Explicit {
			if p := drow.GetOptS32(d); p != nil {
				mrow.SetOptS32(model, *p)
			}
			return nil
		}
		mrow.SetS32(model, drow.GetS32(d))
	case KindSint64:
		if mrow.Explicit {
			if p := drow.GetOptS64(d); p != nil {
				mrow.SetOptS64(model, *p)
			}
			return nil
		}
		mrow.SetS64(model, drow.GetS64(d))
	case KindFixed32:
		if mrow.Explicit {
			if p := drow.GetOptFx32(d); p != nil {
				mrow.SetOptFx32(model, *p)
			}
			return nil
		}
		mrow.SetFx32(model, drow.GetFx32(d))
	case KindFixed64:
		if mrow.Explicit {
			if p := drow.GetOptFx64(d); p != nil {
				mrow.SetOptFx64(model, *p)
			}
			return nil
		}
		mrow.SetFx64(model, drow.GetFx64(d))
	case KindSfixed32:
		if mrow.Explicit {
			if p := drow.GetOptSfx32(d); p != nil {
				mrow.SetOptSfx32(model, *p)
			}
			return nil
		}
		mrow.SetSfx32(model, drow.GetSfx32(d))
	case KindSfixed64:
		if mrow.Explicit {
			if p := drow.GetOptSfx64(d); p != nil {
				mrow.SetOptSfx64(model, *p)
			}
			return nil
		}
		mrow.SetSfx64(model, drow.GetSfx64(d))
	case KindFloat:
		if mrow.Explicit {
			if p := drow.GetOptF32(d); p != nil {
				mrow.SetOptF32(model, *p)
			}
			return nil
		}
		mrow.SetF32(model, drow.GetF32(d))
	case KindDouble:
		if mrow.Explicit {
			if p := drow.GetOptF64(d); p != nil {
				mrow.SetOptF64(model, *p)
			}
			return nil
		}
		mrow.SetF64(model, drow.GetF64(d))
	case KindBool:
		if mrow.Explicit {
			if p := drow.GetOptBool(d); p != nil {
				mrow.SetOptBool(model, *p)
			}
			return nil
		}
		mrow.SetBool(model, drow.GetBool(d))
	case KindString:
		if mrow.Explicit {
			if p := drow.GetOptStr(d); p != nil {
				mrow.SetOptStr(model, *p)
			}
			return nil
		}
		mrow.SetStr(model, drow.GetStr(d))
	case KindBytes:
		if mrow.Explicit {
			if p := drow.GetOptBytes(d); p != nil {
				mrow.SetOptBytes(model, *p)
			}
			return nil
		}
		mrow.SetBytes(model, drow.GetBytes(d))
	case KindEnum:
		if mrow.Explicit {
			if p := drow.GetOptEnum(d); p != nil {
				mrow.SetOptEnum(model, *p)
			}
			return nil
		}
		mrow.SetEnum(model, drow.GetEnum(d))
	}
	return nil
}

// importRepScalar copies one repeated scalar run element by element.
func importRepScalar(model any, mrow *Field, d *DMsg, drow *Field) error {
	switch mrow.Kind {
	case KindInt32:
		for _, v := range mrow.GetRepI32(model) {
			drow.AppendI32(d, v)
		}
	case KindInt64:
		for _, v := range mrow.GetRepI64(model) {
			drow.AppendI64(d, v)
		}
	case KindUint32:
		for _, v := range mrow.GetRepU32(model) {
			drow.AppendU32(d, v)
		}
	case KindUint64:
		for _, v := range mrow.GetRepU64(model) {
			drow.AppendU64(d, v)
		}
	case KindSint32:
		for _, v := range mrow.GetRepS32(model) {
			drow.AppendS32(d, v)
		}
	case KindSint64:
		for _, v := range mrow.GetRepS64(model) {
			drow.AppendS64(d, v)
		}
	case KindFixed32:
		for _, v := range mrow.GetRepFx32(model) {
			drow.AppendFx32(d, v)
		}
	case KindFixed64:
		for _, v := range mrow.GetRepFx64(model) {
			drow.AppendFx64(d, v)
		}
	case KindSfixed32:
		for _, v := range mrow.GetRepSfx32(model) {
			drow.AppendSfx32(d, v)
		}
	case KindSfixed64:
		for _, v := range mrow.GetRepSfx64(model) {
			drow.AppendSfx64(d, v)
		}
	case KindFloat:
		for _, v := range mrow.GetRepF32(model) {
			drow.AppendF32(d, v)
		}
	case KindDouble:
		for _, v := range mrow.GetRepF64(model) {
			drow.AppendF64(d, v)
		}
	case KindBool:
		for _, v := range mrow.GetRepBool(model) {
			drow.AppendBool(d, v)
		}
	case KindString:
		for _, v := range mrow.GetRepStr(model) {
			drow.AppendStr(d, v)
		}
	case KindBytes:
		for _, v := range mrow.GetRepBytes(model) {
			drow.AppendBytes(d, v)
		}
	case KindEnum:
		for _, v := range mrow.GetRepEnum(model) {
			drow.AppendEnum(d, v)
		}
	}
	return nil
}

// exportRepScalar copies one repeated scalar run element by element.
func exportRepScalar(d *DMsg, drow *Field, model any, mrow *Field) error {
	switch mrow.Kind {
	case KindInt32:
		for _, v := range drow.GetRepI32(d) {
			mrow.AppendI32(model, v)
		}
	case KindInt64:
		for _, v := range drow.GetRepI64(d) {
			mrow.AppendI64(model, v)
		}
	case KindUint32:
		for _, v := range drow.GetRepU32(d) {
			mrow.AppendU32(model, v)
		}
	case KindUint64:
		for _, v := range drow.GetRepU64(d) {
			mrow.AppendU64(model, v)
		}
	case KindSint32:
		for _, v := range drow.GetRepS32(d) {
			mrow.AppendS32(model, v)
		}
	case KindSint64:
		for _, v := range drow.GetRepS64(d) {
			mrow.AppendS64(model, v)
		}
	case KindFixed32:
		for _, v := range drow.GetRepFx32(d) {
			mrow.AppendFx32(model, v)
		}
	case KindFixed64:
		for _, v := range drow.GetRepFx64(d) {
			mrow.AppendFx64(model, v)
		}
	case KindSfixed32:
		for _, v := range drow.GetRepSfx32(d) {
			mrow.AppendSfx32(model, v)
		}
	case KindSfixed64:
		for _, v := range drow.GetRepSfx64(d) {
			mrow.AppendSfx64(model, v)
		}
	case KindFloat:
		for _, v := range drow.GetRepF32(d) {
			mrow.AppendF32(model, v)
		}
	case KindDouble:
		for _, v := range drow.GetRepF64(d) {
			mrow.AppendF64(model, v)
		}
	case KindBool:
		for _, v := range drow.GetRepBool(d) {
			mrow.AppendBool(model, v)
		}
	case KindString:
		for _, v := range drow.GetRepStr(d) {
			mrow.AppendStr(model, v)
		}
	case KindBytes:
		for _, v := range drow.GetRepBytes(d) {
			mrow.AppendBytes(model, v)
		}
	case KindEnum:
		for _, v := range drow.GetRepEnum(d) {
			mrow.AppendEnum(model, v)
		}
	}
	return nil
}
