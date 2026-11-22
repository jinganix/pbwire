// Well-known-type helpers over the dynamic tree, like the C core's wkt.c
// and the Java runtime's Wkt: google.protobuf.Any packing plus
// Timestamp/Duration millisecond conversion and FieldMask paths.
//
// Field numbers only (Any 1/2, seconds/nanos 1/2, paths 1), so any
// descriptor with the same shape works; the helpers never special-case
// WKTs anywhere else. WKT touches no encode/decode hot path.
package runtime

import (
	"fmt"
	"math"
	"strings"

	"google.golang.org/protobuf/types/descriptorpb"
)

// Field numbers the helpers use. Any descriptor with the same shape works;
// the declared WKT field names are never consulted.
const (
	wktAnyTypeURLField = uint32(1)
	wktAnyPayloadField = uint32(2)
	wktSecondsField    = uint32(1)
	wktNanosField      = uint32(2)
	wktPathsField      = uint32(1)
)

// maxNanos is the largest absolute sub-second remainder a Timestamp or
// Duration may carry.
const maxNanos = int64(999999999)

// slotByNum finds the slot, its ordinal and its plan for a field number.
// Shape-agnostic descriptors may name the field anything, so the number
// is the lookup.
func slotByNum(m *DMsg, num uint32) (*slot, int, *FieldPlan, bool) {
	if m == nil {
		return nil, 0, nil, false
	}
	for i, fp := range m.typ.plan.Fields {
		if fp.Number == num {
			return &m.slots[i], i, fp, true
		}
	}
	return nil, 0, nil, false
}

// isIntKind reports whether the field stores integer bits in the slot,
// the only shapes millisecond conversion reads.
func isIntKind(fp *FieldPlan) bool {
	switch fp.Proto {
	case descriptorpb.FieldDescriptorProto_TYPE_INT32,
		descriptorpb.FieldDescriptorProto_TYPE_INT64,
		descriptorpb.FieldDescriptorProto_TYPE_UINT32,
		descriptorpb.FieldDescriptorProto_TYPE_UINT64,
		descriptorpb.FieldDescriptorProto_TYPE_SINT32,
		descriptorpb.FieldDescriptorProto_TYPE_SINT64,
		descriptorpb.FieldDescriptorProto_TYPE_FIXED32,
		descriptorpb.FieldDescriptorProto_TYPE_FIXED64,
		descriptorpb.FieldDescriptorProto_TYPE_SFIXED32,
		descriptorpb.FieldDescriptorProto_TYPE_SFIXED64,
		descriptorpb.FieldDescriptorProto_TYPE_ENUM,
		descriptorpb.FieldDescriptorProto_TYPE_BOOL:
		return true
	}
	return false
}

// AnyPack packs a payload with its type URL into a new Any message.
func AnyPack(anyType *DType, typeURL string, payload []byte) (*DMsg, error) {
	if anyType == nil {
		return nil, fmt.Errorf("codec: AnyPack needs a type")
	}
	m := anyType.New()
	sURL, iURL, fpURL, ok := slotByNum(m, wktAnyTypeURLField)
	if !ok || fpURL.Proto != descriptorpb.FieldDescriptorProto_TYPE_STRING || fpURL.Repeated {
		return nil, fmt.Errorf("codec: AnyPack needs a string field 1")
	}
	sPay, iPay, fpPay, ok := slotByNum(m, wktAnyPayloadField)
	if !ok || fpPay.Proto != descriptorpb.FieldDescriptorProto_TYPE_BYTES || fpPay.Repeated {
		return nil, fmt.Errorf("codec: AnyPack needs a bytes field 2")
	}
	sURL.str = typeURL
	m.mark(iURL, typeURL)
	cp := append([]byte(nil), payload...)
	sPay.p = cp
	m.mark(iPay, cp)
	return m, nil
}

// AnyTypeURL is the type URL of an Any message, or "" when absent.
func AnyTypeURL(m *DMsg) string {
	s, _, fp, ok := slotByNum(m, wktAnyTypeURLField)
	if !ok || !s.present {
		return ""
	}
	if fp.Proto != descriptorpb.FieldDescriptorProto_TYPE_STRING {
		return ""
	}
	return s.str
}

// AnyPayload is a copy of the Any payload, or nil when absent.
func AnyPayload(m *DMsg) []byte {
	s, _, fp, ok := slotByNum(m, wktAnyPayloadField)
	if !ok || !s.present {
		return nil
	}
	if fp.Proto != descriptorpb.FieldDescriptorProto_TYPE_BYTES {
		return nil
	}
	return append([]byte(nil), s.p...)
}

// AnyUnpack decodes the Any payload as typ. A present type URL must name
// that type, matched by suffix after the last slash, so any URL prefix
// works; an empty URL skips the check, like the Java helper.
func AnyUnpack(m *DMsg, typ *DType) (*DMsg, error) {
	if m == nil || typ == nil {
		return nil, fmt.Errorf("codec: AnyUnpack needs a value and a type")
	}
	if url := AnyTypeURL(m); url != "" {
		named := url[strings.LastIndex(url, "/")+1:]
		if named != typ.Plan().Full {
			return nil, fmt.Errorf("codec: Any type %q is not %s", url, typ.Plan().Full)
		}
	}
	s, _, _, ok := slotByNum(m, wktAnyPayloadField)
	if !ok || !s.present {
		return nil, ErrWire
	}
	return typ.Decode(AnyPayload(m))
}

// toMillis reads seconds (field 1) + nanos (field 2) as milliseconds,
// shared by both time shapes: same fields, same range rule. A Timestamp
// carries only non-negative nanos; a Duration keeps the sign in both fields.
func toMillis(m *DMsg, what string, timestamp bool) (int64, error) {
	if m == nil {
		return 0, fmt.Errorf("codec: %s needs a value", what)
	}
	sSec, _, fpSec, ok := slotByNum(m, wktSecondsField)
	if !ok || !sSec.present || !isIntKind(fpSec) {
		return 0, ErrWire
	}
	sec := int64(sSec.u)
	var nanos int64
	if sNano, _, fpNano, ok := slotByNum(m, wktNanosField); ok && sNano.present {
		if !isIntKind(fpNano) {
			return 0, ErrWire
		}
		nanos = int64(int32(sNano.u))
	}
	if nanos > maxNanos || nanos < -maxNanos || (timestamp && nanos < 0) {
		return 0, ErrWire
	}
	// sec*1000 + nanos/1e6 must stay inside int64; each bound uses the
	// actual remainder for its direction, so a legal boundary pair (sec
	// 9223372036854775, nanos 807000000 == MaxInt64) still converts.
	frac := nanos / 1000000
	var hi, lo int64
	if frac > 0 {
		hi = (math.MaxInt64 - frac) / 1000
	} else {
		hi = math.MaxInt64 / 1000
	}
	if frac < 0 {
		lo = (math.MinInt64 - frac) / 1000
	} else {
		lo = math.MinInt64 / 1000
	}
	if sec > hi || sec < lo {
		return 0, ErrLimit
	}
	return sec*1000 + frac, nil
}

// TimestampMillis is a Timestamp message as milliseconds since the epoch.
func TimestampMillis(m *DMsg) (int64, error) {
	return toMillis(m, "Timestamp", true)
}

// DurationMillis is a Duration message as milliseconds.
func DurationMillis(m *DMsg) (int64, error) {
	return toMillis(m, "Duration", false)
}

// fromMillis builds a seconds/nanos message from milliseconds.
// floorNanos borrows a second for a negative remainder so Timestamps keep
// nanos in [0, 1e9); Durations keep the sign in both fields.
func fromMillis(typ *DType, ms int64, floorNanos bool, what string) (*DMsg, error) {
	if typ == nil {
		return nil, fmt.Errorf("codec: %s needs a type", what)
	}
	secs := ms / 1000
	nanos := (ms % 1000) * 1000000
	if floorNanos && nanos < 0 {
		secs--
		nanos += 1000000000
	}
	// The pair must convert back: secs*1000 has to stay inside int64, and
	// the Timestamp borrow can step one second below the range at MinInt64.
	if secs > math.MaxInt64/1000 || secs < math.MinInt64/1000 {
		return nil, ErrLimit
	}
	m := typ.New()
	sSec, iSec, fpSec, ok := slotByNum(m, wktSecondsField)
	if !ok || fpSec.Repeated || !isIntKind(fpSec) {
		return nil, fmt.Errorf("codec: %s needs an integer field 1", what)
	}
	sNano, iNano, fpNano, ok := slotByNum(m, wktNanosField)
	if !ok || fpNano.Repeated || !isIntKind(fpNano) {
		return nil, fmt.Errorf("codec: %s needs an integer field 2", what)
	}
	sSec.u = uint64(secs)
	sNano.u = uint64(int64(int32(nanos)))
	m.mark(iSec, secs)
	m.mark(iNano, int32(nanos))
	return m, nil
}

// TimestampFromMillis is milliseconds since the epoch as a Timestamp.
func TimestampFromMillis(typ *DType, ms int64) (*DMsg, error) {
	return fromMillis(typ, ms, true, "Timestamp")
}

// DurationFromMillis is milliseconds as a Duration; both fields keep the
// sign.
func DurationFromMillis(typ *DType, ms int64) (*DMsg, error) {
	return fromMillis(typ, ms, false, "Duration")
}

// FieldMaskPaths is the paths of a FieldMask message (field 1).
func FieldMaskPaths(m *DMsg) []string {
	s, _, fp, ok := slotByNum(m, wktPathsField)
	if !ok || !s.present {
		return nil
	}
	if !fp.Repeated || fp.Proto != descriptorpb.FieldDescriptorProto_TYPE_STRING {
		return nil
	}
	return append([]string(nil), repOf[string](s)...)
}

// NewFieldMask is a FieldMask message holding these paths.
func NewFieldMask(typ *DType, paths []string) *DMsg {
	if typ == nil {
		return nil
	}
	m := typ.New()
	s, _, fp, ok := slotByNum(m, wktPathsField)
	if !ok || !fp.Repeated || fp.Proto != descriptorpb.FieldDescriptorProto_TYPE_STRING {
		return m
	}
	for _, p := range paths {
		s.present = true
		appendRep(s, p)
	}
	return m
}
