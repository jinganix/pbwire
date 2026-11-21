// Single-level wire scan, like the core's pbw_visit: one validating tag
// pass, one callback per field, no message tree. Nested messages and packed
// runs arrive as LEN payloads for the caller to recurse into (another Visit
// on the payload); groups arrive as raw bytes from their start tag through
// the matching end tag.
package runtime

// VisitValue is one scanned field: raw wire content, no allocation, no
// value tree.
type VisitValue struct {
	// U64 holds VARINT/FIXED32/FIXED64 raw wire bits (varints unfolded: no
	// zigzag or sign extension applied). Unused for the shapes below.
	U64 uint64
	// Data holds the LEN payload bytes (a nested message, a packed run, a
	// string); for a group it holds the whole field, tag through the
	// matching END_GROUP. It aliases the scanned input, so a value is only
	// valid until the caller mutates or drops the wire. Nil for scalars.
	Data []byte
}

// Visitor is the scan callback: field is nil when the number is unknown to
// the schema. num/wt are what the wire carries: a known field may carry
// another wire type (a packed LEN for a repeated scalar, or a mismatch the
// decoders keep as an unknown field). A non-nil return aborts the scan and
// surfaces as Visit's error.
type Visitor func(field *Field, num uint32, wt WireType, v VisitValue) error

// Visit scans wire without building a message tree: one validating tag
// pass, one callback per field. It returns the wire length on success and
// the failing or aborting field's byte offset otherwise, like the core's
// fail_off. Only MaxBytes and the group-nesting cap apply: the scan itself
// never recurses, so a nested message or packed run arrives whole.
func (t *DType) Visit(wire []byte, lim Limits, fn Visitor) (failOff int, err error) {
	if t == nil || fn == nil {
		return 0, ErrWire
	}
	if err := lim.checkBytes(len(wire)); err != nil {
		return 0, err
	}
	tb := t.tableFor()
	off := 0
	for off < len(wire) {
		at := off
		num, wt, n, err := ConsumeTag(wire[off:])
		if err != nil {
			return at, ErrWire
		}
		if wt == WireEndGroup {
			return at, ErrWire // end group at top level has no opener
		}
		off += n
		var v VisitValue
		switch wt {
		case WireVarint:
			u, m, err := ConsumeVarint(wire[off:])
			if err != nil {
				return at, ErrWire
			}
			v.U64, off = u, off+m
		case WireFixed64:
			u, m, err := ConsumeFixed64(wire[off:])
			if err != nil {
				return at, ErrWire
			}
			v.U64, off = u, off+m
		case WireFixed32:
			u, m, err := ConsumeFixed32(wire[off:])
			if err != nil {
				return at, ErrWire
			}
			v.U64, off = uint64(u), off+m
		case WireLen:
			p, m, err := ConsumeLen(wire[off:])
			if err != nil {
				return at, ErrWire
			}
			v.Data, off = p, off+m
		case WireStartGroup:
			end, err := skipGroupFrame(wire, off, num, 1, lim)
			if err != nil {
				return at, err
			}
			v.Data, off = wire[at:end], end
		default:
			return at, ErrWire
		}
		// A known field carrying another wire type still resolves by
		// number, like the core's fnum fallback; unknown numbers are nil.
		if err := fn(lookup(tb, num), num, wt, v); err != nil {
			return at, err
		}
	}
	return len(wire), nil
}

// skipGroupFrame skips from off (just after a START_GROUP tag) to just past
// the matching END_GROUP tag. Group frames nest, so depth counts frames and
// trips the effective cap, like the core's skip_group; every tag walked past
// is validated, so field 0, bad wire types and mismatched ends fail.
func skipGroupFrame(wire []byte, off int, num uint32, depth int, lim Limits) (int, error) {
	if err := lim.checkDepth(depth); err != nil {
		return 0, err
	}
	for off < len(wire) {
		inum, iwt, n, err := ConsumeTag(wire[off:])
		if err != nil {
			return 0, ErrWire
		}
		off += n
		if iwt == WireEndGroup {
			if inum != num {
				return 0, ErrWire
			}
			return off, nil
		}
		switch iwt {
		case WireVarint:
			_, m, err := ConsumeVarint(wire[off:])
			if err != nil {
				return 0, ErrWire
			}
			off += m
		case WireFixed64:
			if len(wire)-off < 8 {
				return 0, ErrWire
			}
			off += 8
		case WireFixed32:
			if len(wire)-off < 4 {
				return 0, ErrWire
			}
			off += 4
		case WireLen:
			_, m, err := ConsumeLen(wire[off:])
			if err != nil {
				return 0, ErrWire
			}
			off += m
		case WireStartGroup:
			end, err := skipGroupFrame(wire, off, inum, depth+1, lim)
			if err != nil {
				return 0, err
			}
			off = end
		default:
			return 0, ErrWire
		}
	}
	return 0, ErrWire // unclosed group
}
