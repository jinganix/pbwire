// JSON mapping over the dynamic core: prints the canonical encoding the
// reference implementation produces and parses what its parser accepts in
// default (legacy) mode. Mirrors src/json.c, which is the authority for
// every semantic here, and bindings/java/runtime/Json.java.
//
// Field names print as their camelCase (json_name) spelling, extensions as
// [full.name], 64-bit integers quoted, bytes as standard base64,
// enums by name, floats in the shortest round-tripping form (non-finite
// values quoted), maps as objects with duplicate keys collapsed to the last
// entry, and the well-known types in their JSON form (@type expansions,
// RFC 3339 timestamps, "3.2s" durations, Struct as a nested object,
// FieldMask as one camelCase string, wrappers as bare values). Unknown
// fields are not printed.
//
// The parser accepts proto and camelCase names, quoted or bare 64-bit
// integers, numbers with exponential or fractional spelling, quoted bools,
// single-quoted and bare keys, trailing commas, null as unset, nested-array
// flattening (outside Value/ListValue), the object form of well-known types
// ({"seconds": ...}), base64 with either alphabet and optional padding,
// unknown closed-enum numbers diverted to unknown fields (like the binary
// decoder), and duplicate keys except inside oneofs and maps. Anything else
// unknown fails unless told to ignore it.
package runtime

import (
	"encoding/base64"
	"fmt"
	"math"
	"sort"
	"strconv"
	"strings"
	"time"
	"unicode/utf8"

	"google.golang.org/protobuf/types/descriptorpb"
)

// MarshalJSON prints a message in the JSON mapping.
func (t *DType) MarshalJSON(m *DMsg) (string, error) {
	o := &jsonOut{}
	o.printMessage(m)
	if o.err != nil {
		return "", o.err
	}
	return o.sb.String(), nil
}

// UnmarshalJSON parses JSON into a new tree; unknown fields fail unless
// ignoreUnknown is set. An unresolvable Any @type always fails, and a
// missing proto2 required field fails after the parse.
func (t *DType) UnmarshalJSON(text string, ignoreUnknown bool) (*DMsg, error) {
	return t.UnmarshalJSONLimits(text, ignoreUnknown, Limits{})
}

// UnmarshalJSONLimits is UnmarshalJSON with explicit bounds: documents
// larger than the byte cap fail before anything parses, and nesting past
// the effective cap fails. Like the reference, an array costs a level too,
// so a repeated message chain spends two levels per message. The old name
// keeps its signature as a defaults wrapper.
func (t *DType) UnmarshalJSONLimits(text string, ignoreUnknown bool, lim Limits) (*DMsg, error) {
	if err := lim.checkBytes(len(text)); err != nil {
		return nil, err
	}
	m := t.New()
	p := &jsonIn{s: text, ignore: ignoreUnknown, lim: lim}
	if err := p.parseMessageValue(m); err != nil {
		return nil, err
	}
	p.skip()
	if p.pos != len(p.s) {
		return nil, fmt.Errorf("json: trailing bytes at offset %d", p.pos)
	}
	if err := jCheckRequired(m); err != nil {
		return nil, err
	}
	return m, nil
}

// protoc's ToJsonName: drops every '_', uppercases the next letter.
// jsonFieldName returns the field's JSON name: memoized at plan build,
// computed on the fly for hand-built plans that carry none.
func jsonFieldName(fp *FieldPlan) string {
	if fp.JSONName != "" {
		return fp.JSONName
	}
	return jsonName(fp.Name)
}

func jsonName(name string) string {
	var sb strings.Builder
	sb.Grow(len(name))
	upper := false
	for i := 0; i < len(name); i++ {
		c := name[i]
		if c == '_' {
			upper = true
			continue
		}
		if upper {
			c = upperASCII(c)
			upper = false
		}
		sb.WriteByte(c)
	}
	return sb.String()
}

// protoc's ToCamelCase with a lowercased first letter: the parse spelling.
func camelName(name string) string {
	var sb strings.Builder
	sb.Grow(len(name))
	upper := true
	for i := 0; i < len(name); i++ {
		c := name[i]
		if c == '_' {
			upper = true
			continue
		}
		if upper {
			c = upperASCII(c)
			upper = false
		}
		if sb.Len() == 0 {
			c = lowerASCII(c)
		}
		sb.WriteByte(c)
	}
	return sb.String()
}

func upperASCII(c byte) byte {
	if c >= 'a' && c <= 'z' {
		return c - 'a' + 'A'
	}
	return c
}

func lowerASCII(c byte) byte {
	if c >= 'A' && c <= 'Z' {
		return c - 'A' + 'a'
	}
	return c
}

// Well-known types by full message name. NullValue is an enum, routed with
// the same table for the null spelling.
type jsonWKT int

const (
	jwNone jsonWKT = iota
	jwAny
	jwTimestamp
	jwDuration
	jwStruct
	jwValue
	jwList
	jwFieldMask
	jwWrapper
	jwEmpty
)

const jsonNullValue = "google.protobuf.NullValue"

func jsonClassify(full string) jsonWKT {
	switch full {
	case "google.protobuf.Any":
		return jwAny
	case "google.protobuf.Timestamp":
		return jwTimestamp
	case "google.protobuf.Duration":
		return jwDuration
	case "google.protobuf.Struct":
		return jwStruct
	case "google.protobuf.Value":
		return jwValue
	case "google.protobuf.ListValue":
		return jwList
	case "google.protobuf.FieldMask":
		return jwFieldMask
	case "google.protobuf.DoubleValue",
		"google.protobuf.FloatValue",
		"google.protobuf.Int64Value",
		"google.protobuf.UInt64Value",
		"google.protobuf.Int32Value",
		"google.protobuf.UInt32Value",
		"google.protobuf.BoolValue",
		"google.protobuf.StringValue",
		"google.protobuf.BytesValue":
		return jwWrapper
	case "google.protobuf.Empty":
		return jwEmpty
	}
	return jwNone
}

// ordinalOf finds a field plan's slot ordinal by pointer identity.
func ordinalOf(plan *MessagePlan, fp *FieldPlan) int {
	for i, f := range plan.Fields {
		if f == fp {
			return i
		}
	}
	return -1
}

// fieldByNumber resolves a field plan plus its ordinal, or nil when absent.
// The ordinal rides the memoized index; hand-built plans without one fall
// back to the pointer scan.
func fieldByNumber(plan *MessagePlan, num uint32) (*FieldPlan, int) {
	fp, ok := plan.ByNumber(num)
	if !ok {
		return nil, -1
	}
	if j, ok := plan.ordByNum[num]; ok {
		return fp, j
	}
	return fp, ordinalOf(plan, fp)
}

/* ------------------------------------------------------------ printer */

type jsonOut struct {
	sb    strings.Builder
	err   error
	depth int
}

// enter spends one nesting level; past MaxDepth the printer fails with
// ErrDepth instead of growing the goroutine stack.
func (o *jsonOut) enter() bool {
	if o.err != nil {
		return false
	}
	if o.depth >= MaxDepth {
		o.fail(ErrDepth)
		return false
	}
	o.depth++
	return true
}

func (o *jsonOut) fail(err error) {
	if o.err == nil {
		o.err = err
	}
}

func (o *jsonOut) str(s string) {
	if o.err == nil {
		o.sb.WriteString(s)
	}
}

func (o *jsonOut) ch(c byte) {
	if o.err == nil {
		o.sb.WriteByte(c)
	}
}

// quoteContent writes a string's escaped body without the quotes.
func (o *jsonOut) quoteContent(s string) {
	if o.err != nil {
		return
	}
	if !utf8.ValidString(s) {
		o.fail(fmt.Errorf("json: string is not valid UTF-8"))
		return
	}
	const hex = "0123456789abcdef"
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch c {
		case '"':
			o.str("\\\"")
		case '\\':
			o.str("\\\\")
		case '\b':
			o.str("\\b")
		case '\f':
			o.str("\\f")
		case '\n':
			o.str("\\n")
		case '\r':
			o.str("\\r")
		case '\t':
			o.str("\\t")
		default:
			switch {
			case c == '<':
				o.str("\\u003c")
			case c == '>':
				o.str("\\u003e")
			case c < 0x20 || c == 0x7f:
				o.str("\\u00")
				o.ch(hex[c>>4])
				o.ch(hex[c&15])
			default:
				o.ch(c)
			}
		}
	}
}

func (o *jsonOut) quoted(s string) {
	o.ch('"')
	o.quoteContent(s)
	o.ch('"')
}

// repLen reports a repeated slot's element count.
func repLen(s *slot) int {
	switch t := s.rep.(type) {
	case []int32:
		return len(t)
	case []int64:
		return len(t)
	case []uint32:
		return len(t)
	case []uint64:
		return len(t)
	case []float32:
		return len(t)
	case []float64:
		return len(t)
	case []bool:
		return len(t)
	case []string:
		return len(t)
	case [][]byte:
		return len(t)
	case []*DMsg:
		return len(t)
	}
	return 0
}

// isDefault reports whether a present implicit-presence scalar holds its
// zero value and is therefore omitted from the print.
func isDefault(fp *FieldPlan, s *slot) bool {
	switch fp.Proto {
	case descriptorpb.FieldDescriptorProto_TYPE_STRING:
		return s.str == ""
	case descriptorpb.FieldDescriptorProto_TYPE_BYTES:
		return len(s.p) == 0
	default:
		return s.u == 0
	}
}

func (o *jsonOut) shouldPrint(m *DMsg, i int, fp *FieldPlan) bool {
	s := &m.slots[i]
	if !s.present {
		return false
	}
	if fp.Repeated {
		return repLen(s) > 0
	}
	if fp.Proto == descriptorpb.FieldDescriptorProto_TYPE_MESSAGE || fp.Group {
		return true
	}
	if fp.Presence == PresenceOneof {
		return true
	}
	if fp.Presence == PresenceImplicit {
		return !isDefault(fp, s)
	}
	return true
}

func (o *jsonOut) printMessage(m *DMsg) {
	if o.err != nil || m == nil || m.typ == nil {
		o.fail(fmt.Errorf("json: cannot print a nil message"))
		return
	}
	if !o.enter() {
		return
	}
	defer func() { o.depth-- }()
	switch jsonClassify(m.typ.plan.Full) {
	case jwAny:
		o.printAny(m)
		return
	case jwTimestamp:
		o.printTimestamp(m)
		return
	case jwDuration:
		o.printDuration(m)
		return
	case jwFieldMask:
		o.printFieldMask(m)
		return
	case jwStruct:
		o.printStruct(m)
		return
	case jwList:
		o.printListValue(m)
		return
	case jwValue:
		o.printValue(m)
		return
	case jwWrapper:
		o.printWrapper(m)
		return
	case jwEmpty:
		o.str("{}")
		return
	}
	order := m.typ.plan.numOrder
	if order == nil {
		// Hand-built plan without a memoized order: collect and sort.
		order = make([]int, 0, len(m.typ.plan.Fields))
		for i, fp := range m.typ.plan.Fields {
			if o.shouldPrint(m, i, fp) {
				order = append(order, i)
			}
		}
		sort.Slice(order, func(a, b int) bool {
			return m.typ.plan.Fields[order[a]].Number < m.typ.plan.Fields[order[b]].Number
		})
	} else {
		// Memoized number order, filtered by presence per message.
		// The kept list needs its own backing: numOrder is shared
		// across messages (and recursion), so filtering in place
		// would corrupt it. Small messages filter on the stack.
		var stack [32]int
		kept := stack[:0]
		if len(order) > len(stack) {
			kept = make([]int, 0, len(order))
		}
		for _, i := range order {
			if o.shouldPrint(m, i, m.typ.plan.Fields[i]) {
				kept = append(kept, i)
			}
		}
		order = kept
	}
	o.ch('{')
	n := 0
	for _, i := range order {
		fp := m.typ.plan.Fields[i]
		if n > 0 {
			o.ch(',')
		}
		n++
		o.quoted(jsonFieldName(fp))
		o.ch(':')
		if fp.Repeated {
			o.printRepeated(m, i, fp)
			continue
		}
		o.printSingular(m, i, fp)
	}
	// Extensions follow the regular fields in number order, keyed
	// "[full.name]" like the reference printer.
	for x := range m.ext {
		xv := &m.ext[x]
		if !xv.s.present {
			continue
		}
		if n > 0 {
			o.ch(',')
		}
		n++
		o.quoted("[" + xv.xp.Full + "]")
		o.ch(':')
		o.printExtension(m.typ, xv)
	}
	o.ch('}')
}

// printExtension prints one set extension's value: an array for repeated
// runs, the bare value otherwise.
func (o *jsonOut) printExtension(t *DType, xv *extValue) {
	fp, s := extField(xv), &xv.s
	if !fp.Repeated {
		o.printScalar(t, fp, s.u, s.str, s.p, s.m)
		return
	}
	o.ch('[')
	for k := 0; k < repLen(s); k++ {
		if k > 0 {
			o.ch(',')
		}
		o.printElement(t, fp, s, k)
	}
	o.ch(']')
}

// enumName resolves an enum value to its first-declared name.
func enumName(t *DType, fp *FieldPlan, num int32) (string, bool) {
	ep, err := t.s.Enum(fp.Enum)
	if err != nil {
		return "", false
	}
	name, ok := ep.Values[num]
	return name, ok
}

func (o *jsonOut) printFloat(kind descriptorpb.FieldDescriptorProto_Type, bits uint64) {
	if kind == descriptorpb.FieldDescriptorProto_TYPE_FLOAT {
		f := math.Float32frombits(uint32(bits))
		switch {
		case math.IsNaN(float64(f)):
			o.str("\"NaN\"")
		case math.IsInf(float64(f), 1):
			o.str("\"Infinity\"")
		case math.IsInf(float64(f), -1):
			o.str("\"-Infinity\"")
		default:
			o.str(jsonFloat32(f))
		}
		return
	}
	v := math.Float64frombits(bits)
	switch {
	case math.IsNaN(v):
		o.str("\"NaN\"")
	case math.IsInf(v, 1):
		o.str("\"Infinity\"")
	case math.IsInf(v, -1):
		o.str("\"-Infinity\"")
	default:
		o.str(jsonFloat64(v))
	}
}

// jsonFloat64 spells a double the way the reference printer does: the
// shortest %.*g form that parses back to the same value, starting at 15
// significant digits. Go's 'g' with precision -1 switches to an exponent
// spelling sooner than the reference, which turns 123456789 into
// 1.23456789e+08.
func jsonFloat64(v float64) string {
	for prec := 15; prec <= 17; prec++ {
		txt := strconv.FormatFloat(v, 'g', prec, 64)
		if back, err := strconv.ParseFloat(txt, 64); err == nil && back == v {
			return txt
		}
	}
	return "0"
}

// jsonFloat32 is jsonFloat64 for a float value: the reference formats the
// widened double, so 0.1 prints as 0.100000001.
func jsonFloat32(f float32) string {
	for prec := 9; prec <= 17; prec++ {
		txt := strconv.FormatFloat(float64(f), 'g', prec, 64)
		if back, err := strconv.ParseFloat(txt, 64); err == nil && float32(back) == f {
			return txt
		}
	}
	return "0"
}

func (o *jsonOut) printScalar(t *DType, fp *FieldPlan, u uint64, str string, p []byte, sub *DMsg) {
	switch fp.Proto {
	case descriptorpb.FieldDescriptorProto_TYPE_DOUBLE,
		descriptorpb.FieldDescriptorProto_TYPE_FLOAT:
		o.printFloat(fp.Proto, u)
	case descriptorpb.FieldDescriptorProto_TYPE_INT64,
		descriptorpb.FieldDescriptorProto_TYPE_SINT64,
		descriptorpb.FieldDescriptorProto_TYPE_SFIXED64:
		o.ch('"')
		o.str(strconv.FormatInt(int64(u), 10))
		o.ch('"')
	case descriptorpb.FieldDescriptorProto_TYPE_UINT64,
		descriptorpb.FieldDescriptorProto_TYPE_FIXED64:
		o.ch('"')
		o.str(strconv.FormatUint(u, 10))
		o.ch('"')
	case descriptorpb.FieldDescriptorProto_TYPE_INT32,
		descriptorpb.FieldDescriptorProto_TYPE_SINT32,
		descriptorpb.FieldDescriptorProto_TYPE_SFIXED32:
		o.str(strconv.FormatInt(int64(int32(u)), 10))
	case descriptorpb.FieldDescriptorProto_TYPE_UINT32,
		descriptorpb.FieldDescriptorProto_TYPE_FIXED32:
		o.str(strconv.FormatUint(uint64(uint32(u)), 10))
	case descriptorpb.FieldDescriptorProto_TYPE_BOOL:
		if u != 0 {
			o.str("true")
		} else {
			o.str("false")
		}
	case descriptorpb.FieldDescriptorProto_TYPE_STRING:
		o.quoted(str)
	case descriptorpb.FieldDescriptorProto_TYPE_BYTES:
		o.ch('"')
		o.str(base64.StdEncoding.EncodeToString(p))
		o.ch('"')
	case descriptorpb.FieldDescriptorProto_TYPE_ENUM:
		if fp.Enum == jsonNullValue {
			o.str("null")
			return
		}
		if name, ok := enumName(t, fp, int32(u)); ok {
			o.quoted(name)
			return
		}
		o.str(strconv.FormatInt(int64(int32(u)), 10))
	case descriptorpb.FieldDescriptorProto_TYPE_MESSAGE:
		if sub != nil {
			o.printMessage(sub)
		} else {
			o.str("{}")
		}
	default: // groups ride the message path
		if sub != nil {
			o.printMessage(sub)
		} else {
			o.str("{}")
		}
	}
}

func (o *jsonOut) printSingular(m *DMsg, i int, fp *FieldPlan) {
	s := &m.slots[i]
	o.printScalar(m.typ, fp, s.u, s.str, s.p, s.m)
}

func (o *jsonOut) printElement(t *DType, fp *FieldPlan, s *slot, i int) {
	switch fp.Proto {
	case descriptorpb.FieldDescriptorProto_TYPE_INT32,
		descriptorpb.FieldDescriptorProto_TYPE_SINT32,
		descriptorpb.FieldDescriptorProto_TYPE_SFIXED32,
		descriptorpb.FieldDescriptorProto_TYPE_ENUM:
		o.printScalar(t, fp, uint64(int64(s.rep.([]int32)[i])), "", nil, nil)
	case descriptorpb.FieldDescriptorProto_TYPE_INT64,
		descriptorpb.FieldDescriptorProto_TYPE_SINT64,
		descriptorpb.FieldDescriptorProto_TYPE_SFIXED64:
		o.printScalar(t, fp, uint64(s.rep.([]int64)[i]), "", nil, nil)
	case descriptorpb.FieldDescriptorProto_TYPE_UINT32,
		descriptorpb.FieldDescriptorProto_TYPE_FIXED32:
		o.printScalar(t, fp, uint64(s.rep.([]uint32)[i]), "", nil, nil)
	case descriptorpb.FieldDescriptorProto_TYPE_UINT64,
		descriptorpb.FieldDescriptorProto_TYPE_FIXED64:
		o.printScalar(t, fp, s.rep.([]uint64)[i], "", nil, nil)
	case descriptorpb.FieldDescriptorProto_TYPE_FLOAT:
		f := s.rep.([]float32)[i]
		o.printScalar(t, fp, uint64(math.Float32bits(f)), "", nil, nil)
	case descriptorpb.FieldDescriptorProto_TYPE_DOUBLE:
		f := s.rep.([]float64)[i]
		o.printScalar(t, fp, math.Float64bits(f), "", nil, nil)
	case descriptorpb.FieldDescriptorProto_TYPE_BOOL:
		var u uint64
		if s.rep.([]bool)[i] {
			u = 1
		}
		o.printScalar(t, fp, u, "", nil, nil)
	case descriptorpb.FieldDescriptorProto_TYPE_STRING:
		o.printScalar(t, fp, 0, s.rep.([]string)[i], nil, nil)
	case descriptorpb.FieldDescriptorProto_TYPE_BYTES:
		o.printScalar(t, fp, 0, "", s.rep.([][]byte)[i], nil)
	default: // messages and groups
		items := s.rep.([]*DMsg)
		if items[i] != nil {
			o.printMessage(items[i])
		} else {
			o.str("{}")
		}
	}
}

// mapKeyString renders a map key in its JSON object-key spelling.
func mapKeyString(t *DType, fp *FieldPlan, kind descriptorpb.FieldDescriptorProto_Type, u uint64, str string) string {
	switch kind {
	case descriptorpb.FieldDescriptorProto_TYPE_INT32,
		descriptorpb.FieldDescriptorProto_TYPE_SINT32,
		descriptorpb.FieldDescriptorProto_TYPE_SFIXED32,
		descriptorpb.FieldDescriptorProto_TYPE_INT64,
		descriptorpb.FieldDescriptorProto_TYPE_SINT64,
		descriptorpb.FieldDescriptorProto_TYPE_SFIXED64:
		return strconv.FormatInt(int64(u), 10)
	case descriptorpb.FieldDescriptorProto_TYPE_UINT32,
		descriptorpb.FieldDescriptorProto_TYPE_FIXED32:
		return strconv.FormatUint(uint64(uint32(u)), 10)
	case descriptorpb.FieldDescriptorProto_TYPE_UINT64,
		descriptorpb.FieldDescriptorProto_TYPE_FIXED64:
		return strconv.FormatUint(u, 10)
	case descriptorpb.FieldDescriptorProto_TYPE_BOOL:
		if u != 0 {
			return "true"
		}
		return "false"
	case descriptorpb.FieldDescriptorProto_TYPE_STRING:
		return str
	case descriptorpb.FieldDescriptorProto_TYPE_ENUM:
		if name, ok := enumName(t, fp, int32(u)); ok {
			return name
		}
		return strconv.FormatInt(int64(int32(u)), 10)
	}
	return ""
}

// entryKey splits one map entry into its key spelling and value slot facts.
func entryKey(t *DType, entry *DMsg, keyOrd int, keyFP *FieldPlan) string {
	if keyOrd < 0 || keyFP == nil {
		return ""
	}
	ks := &entry.slots[keyOrd]
	if !ks.present {
		return mapKeyString(t, keyFP, keyFP.Proto, 0, "")
	}
	return mapKeyString(t, keyFP, keyFP.Proto, ks.u, ks.str)
}

func (o *jsonOut) printMap(m *DMsg, i int, fp *FieldPlan) {
	entryT, err := m.typ.sub(i)
	if err != nil {
		o.fail(fmt.Errorf("json: map entry type: %w", err))
		return
	}
	keyFP, keyOrd := fieldByNumber(entryT.plan, 1)
	valFP, valOrd := fieldByNumber(entryT.plan, 2)
	if keyFP == nil || valFP == nil {
		o.fail(fmt.Errorf("json: map entry %s has no key/value", entryT.plan.Full))
		return
	}
	items, _ := m.slots[i].rep.([]*DMsg)
	// Binary input may repeat a key (last wins); JSON objects cannot.
	last := map[string]int{}
	for k, e := range items {
		if e == nil {
			o.fail(fmt.Errorf("json: map entry is nil"))
			return
		}
		last[entryKey(entryT, e, keyOrd, keyFP)] = k
	}
	o.ch('{')
	first := true
	for k, e := range items {
		val := &e.slots[valOrd]
		if last[entryKey(entryT, e, keyOrd, keyFP)] != k {
			continue
		}
		if valFP.Proto == descriptorpb.FieldDescriptorProto_TYPE_MESSAGE && val.m != nil &&
			jsonClassify(val.m.typ.plan.Full) == jwValue && jValueIsEmpty(val.m) {
			continue
		}
		if !first {
			o.ch(',')
		}
		first = false
		o.quoted(entryKey(entryT, e, keyOrd, keyFP))
		o.ch(':')
		if !val.present {
			switch valFP.Proto {
			case descriptorpb.FieldDescriptorProto_TYPE_STRING:
				o.quoted("")
			case descriptorpb.FieldDescriptorProto_TYPE_BYTES:
				o.str("\"\"")
			case descriptorpb.FieldDescriptorProto_TYPE_MESSAGE:
				o.str("{}")
			default:
				if valFP.Group {
					o.str("{}")
				} else {
					o.printScalar(entryT, valFP, 0, "", nil, nil)
				}
			}
			continue
		}
		if valFP.Proto == descriptorpb.FieldDescriptorProto_TYPE_MESSAGE || valFP.Group {
			if val.m != nil {
				o.printMessage(val.m)
			} else {
				o.str("{}")
			}
			continue
		}
		o.printScalar(entryT, valFP, val.u, val.str, val.p, nil)
	}
	o.ch('}')
}

func (o *jsonOut) printRepeated(m *DMsg, i int, fp *FieldPlan) {
	s := &m.slots[i]
	if fp.IsMap {
		o.printMap(m, i, fp)
		return
	}
	n := repLen(s)
	o.ch('[')
	wrote := false
	for k := 0; k < n; k++ {
		if fp.Proto == descriptorpb.FieldDescriptorProto_TYPE_MESSAGE && !fp.Group {
			var sub *DMsg
			if items, ok := s.rep.([]*DMsg); ok && k < len(items) {
				sub = items[k]
			}
			if sub != nil && jsonClassify(sub.typ.plan.Full) == jwValue && jValueIsEmpty(sub) {
				continue
			}
		}
		if wrote {
			o.ch(',')
		}
		wrote = true
		o.printElement(m.typ, fp, s, k)
	}
	o.ch(']')
}

// jValueIsEmpty reports a Value holding no set field.
func jValueIsEmpty(m *DMsg) bool {
	for i := range m.typ.plan.Fields {
		if m.slots[i].present {
			return false
		}
	}
	return true
}

func (o *jsonOut) printTimestamp(m *DMsg) {
	secsFP, secsOrd := fieldByNumber(m.typ.plan, 1)
	nanosFP, nanosOrd := fieldByNumber(m.typ.plan, 2)
	if secsFP == nil || nanosFP == nil {
		o.fail(fmt.Errorf("json: Timestamp has no seconds/nanos"))
		return
	}
	secs := int64(m.slots[secsOrd].u)
	nanos := int32(m.slots[nanosOrd].u)
	if secs < -62135596800 || secs > 253402300799 || nanos < 0 || nanos > 999999999 {
		o.fail(fmt.Errorf("json: Timestamp out of range"))
		return
	}
	tm := time.Unix(secs, 0).UTC()
	var sb strings.Builder
	fmt.Fprintf(&sb, "\"%04d-%02d-%02dT%02d:%02d:%02d",
		tm.Year(), int(tm.Month()), tm.Day(), tm.Hour(), tm.Minute(), tm.Second())
	if nanos != 0 {
		frac, digits := uint32(nanos), 9
		for frac%1000 == 0 {
			frac /= 1000
			digits -= 3
		}
		fmt.Fprintf(&sb, ".%0*d", digits, frac)
	}
	sb.WriteString("Z\"")
	o.str(sb.String())
}

func (o *jsonOut) printDuration(m *DMsg) {
	_, secsOrd := fieldByNumber(m.typ.plan, 1)
	_, nanosOrd := fieldByNumber(m.typ.plan, 2)
	if secsOrd < 0 || nanosOrd < 0 {
		o.fail(fmt.Errorf("json: Duration has no seconds/nanos"))
		return
	}
	secs := int64(m.slots[secsOrd].u)
	nanos := int32(m.slots[nanosOrd].u)
	const max = int64(3652500) * 86400
	if secs > max || secs < -max || nanos > 999999999 || nanos < -999999999 ||
		(secs != 0 && nanos != 0 && (secs < 0) != (nanos < 0)) {
		o.fail(fmt.Errorf("json: Duration out of range"))
		return
	}
	if nanos == 0 {
		o.str("\"" + strconv.FormatInt(secs, 10) + "s\"")
		return
	}
	frac, digits := uint32(nanos), 9
	if nanos < 0 {
		frac = uint32(-nanos)
	}
	for frac%1000 == 0 {
		frac /= 1000
		digits -= 3
	}
	var sb strings.Builder
	sb.WriteString("\"")
	if secs < 0 || nanos < 0 {
		sb.WriteString("-")
	}
	a := secs
	if a < 0 {
		a = -a
	}
	fmt.Fprintf(&sb, "%d.%0*d", a, digits, frac)
	sb.WriteString("s\"")
	o.str(sb.String())
}

func (o *jsonOut) printFieldMask(m *DMsg) {
	_, pathsOrd := fieldByNumber(m.typ.plan, 1)
	if pathsOrd < 0 {
		o.fail(fmt.Errorf("json: FieldMask has no paths"))
		return
	}
	paths, _ := m.slots[pathsOrd].rep.([]string)
	var sb strings.Builder
	for k, p := range paths {
		if k > 0 {
			sb.WriteString(",")
		}
		sawUnder := false
		for j := 0; j < len(p); j++ {
			c := p[j]
			switch {
			case c >= 'a' && c <= 'z' && sawUnder:
				sb.WriteByte(c - 'a' + 'A')
			case (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.':
				sb.WriteByte(c)
			case c == '_':
				sawUnder = true
				continue
			default:
				if sawUnder {
					sb.WriteByte('_')
				}
				sb.WriteByte(c)
			}
			sawUnder = false
		}
	}
	o.quoted(sb.String())
}

func (o *jsonOut) printStruct(m *DMsg) {
	if !o.enter() {
		return
	}
	defer func() { o.depth-- }()
	_, fieldsOrd := fieldByNumber(m.typ.plan, 1)
	if fieldsOrd < 0 {
		o.fail(fmt.Errorf("json: Struct has no fields"))
		return
	}
	items, _ := m.slots[fieldsOrd].rep.([]*DMsg)
	// Binary input may repeat a key (last wins); a JSON object cannot.
	last := make(map[string]int, len(items))
	for k, e := range items {
		if e == nil {
			o.fail(fmt.Errorf("json: Struct entry is nil"))
			return
		}
		keyFP, keyOrd := fieldByNumber(e.typ.plan, 1)
		if keyFP == nil || !e.slots[keyOrd].present {
			o.fail(fmt.Errorf("json: Struct entry is malformed"))
			return
		}
		last[e.slots[keyOrd].str] = k
	}
	o.ch('{')
	first := true
	for k, e := range items {
		_, keyOrd := fieldByNumber(e.typ.plan, 1)
		valFP, valOrd := fieldByNumber(e.typ.plan, 2)
		if keyOrd < 0 || valFP == nil || e.slots[valOrd].m == nil {
			o.fail(fmt.Errorf("json: Struct entry is malformed"))
			return
		}
		val := e.slots[valOrd].m
		if last[e.slots[keyOrd].str] != k {
			continue
		}
		if jsonClassify(val.typ.plan.Full) == jwValue && jValueIsEmpty(val) {
			continue
		}
		if !first {
			o.ch(',')
		}
		first = false
		o.quoted(e.slots[keyOrd].str)
		o.ch(':')
		o.printValue(val)
	}
	o.ch('}')
}

func (o *jsonOut) printListValue(m *DMsg) {
	if !o.enter() {
		return
	}
	defer func() { o.depth-- }()
	_, valuesOrd := fieldByNumber(m.typ.plan, 1)
	if valuesOrd < 0 {
		o.fail(fmt.Errorf("json: ListValue has no values"))
		return
	}
	items, _ := m.slots[valuesOrd].rep.([]*DMsg)
	o.ch('[')
	first := true
	for _, item := range items {
		if item == nil {
			o.fail(fmt.Errorf("json: ListValue item is nil"))
			return
		}
		if jsonClassify(item.typ.plan.Full) == jwValue && jValueIsEmpty(item) {
			continue
		}
		if !first {
			o.ch(',')
		}
		first = false
		o.printValue(item)
	}
	o.ch(']')
}

// printValue prints a google.protobuf.Value: exactly one of its six fields.
func (o *jsonOut) printValue(m *DMsg) {
	if !o.enter() {
		return
	}
	defer func() { o.depth-- }()
	for n := uint32(1); n <= 6; n++ {
		fp, ord := fieldByNumber(m.typ.plan, n)
		if fp == nil || !m.slots[ord].present {
			continue
		}
		s := &m.slots[ord]
		switch n {
		case 1: // null_value
			o.str("null")
		case 2: // number_value
			v := math.Float64frombits(s.u)
			if math.IsNaN(v) || math.IsInf(v, 0) {
				o.fail(fmt.Errorf("json: Value number is not finite"))
				return
			}
			o.str(jsonFloat64(v))
		case 3: // string_value
			o.quoted(s.str)
		case 4: // bool_value
			if s.u != 0 {
				o.str("true")
			} else {
				o.str("false")
			}
		case 5: // struct_value
			if s.m == nil {
				o.fail(fmt.Errorf("json: Value struct is nil"))
				return
			}
			o.printStruct(s.m)
		default: // list_value
			if s.m == nil {
				o.fail(fmt.Errorf("json: Value list is nil"))
				return
			}
			o.printListValue(s.m)
		}
		return
	}
	o.fail(fmt.Errorf("json: google.protobuf.Value has no value"))
}

func (o *jsonOut) printWrapper(m *DMsg) {
	fp, ord := fieldByNumber(m.typ.plan, 1)
	if fp == nil {
		o.fail(fmt.Errorf("json: wrapper has no value field"))
		return
	}
	s := &m.slots[ord]
	if !s.present {
		switch fp.Proto {
		case descriptorpb.FieldDescriptorProto_TYPE_STRING:
			o.quoted("")
		case descriptorpb.FieldDescriptorProto_TYPE_BYTES:
			o.str("\"\"")
		default:
			o.printScalar(m.typ, fp, 0, "", nil, nil)
		}
		return
	}
	o.printScalar(m.typ, fp, s.u, s.str, s.p, nil)
}

func (o *jsonOut) printAny(m *DMsg) {
	_, urlOrd := fieldByNumber(m.typ.plan, 1)
	_, valOrd := fieldByNumber(m.typ.plan, 2)
	if urlOrd < 0 || valOrd < 0 {
		o.fail(fmt.Errorf("json: Any has no type_url/value"))
		return
	}
	if !m.slots[urlOrd].present {
		if m.slots[valOrd].present {
			o.fail(fmt.Errorf("json: Any has a value without a type"))
			return
		}
		o.str("{}")
		return
	}
	url := m.slots[urlOrd].str
	name := url
	if k := strings.LastIndexByte(url, '/'); k >= 0 {
		name = url[k+1:]
	}
	sub, err := m.typ.s.Type(name)
	if err != nil {
		o.fail(fmt.Errorf("json: Any type is not resolvable: %s", url))
		return
	}
	decoded, err := sub.Decode(m.slots[valOrd].p)
	if err != nil {
		o.fail(fmt.Errorf("json: Any payload does not decode as %s", name))
		return
	}
	o.str("{\"@type\":")
	o.quoted(url)
	kind := jsonClassify(sub.plan.Full)
	if kind != jwNone && kind != jwEmpty {
		o.str(",\"value\":")
		o.printMessage(decoded)
	} else if !jValueIsEmpty(decoded) {
		mark := o.sb.Len()
		o.printMessage(decoded)
		if o.err != nil {
			return
		}
		s := o.sb.String()
		if len(s) >= mark+2 && s[mark] == '{' && s[len(s)-1] == '}' {
			o.sb.Reset()
			o.sb.WriteString(s[:mark])
			o.sb.WriteByte(',')
			o.sb.WriteString(s[mark+1 : len(s)-1])
		}
	}
	o.ch('}')
}

/* ------------------------------------------------------------- parser */

type jsonKind int

const (
	jkEnd jsonKind = iota
	jkObj
	jkArr
	jkStr
	jkNum
	jkTrue
	jkFalse
	jkNull
	jkBad
)

type jsonIn struct {
	s      string
	pos    int
	ignore bool
	lim    Limits
	depth  int
}

// enter counts one nesting level: a message, or an array, which costs a
// level like the reference. Past the effective cap it reports ErrDepth.
func (p *jsonIn) enter() error {
	if p.depth >= p.lim.effectiveDepth() {
		return ErrDepth
	}
	p.depth++
	return nil
}

func (p *jsonIn) fail(format string, args ...any) error {
	return fmt.Errorf("json: "+format, args...)
}

func (p *jsonIn) skip() {
	for p.pos < len(p.s) {
		switch p.s[p.pos] {
		case ' ', '\t', '\n', '\r':
			p.pos++
		default:
			return
		}
	}
}

func (p *jsonIn) peekKind() jsonKind {
	p.skip()
	if p.pos >= len(p.s) {
		return jkEnd
	}
	switch p.s[p.pos] {
	case '{':
		return jkObj
	case '[':
		return jkArr
	case '"', '\'':
		return jkStr
	case 't':
		return jkTrue
	case 'f':
		return jkFalse
	case 'n':
		return jkNull
	case '-', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9':
		return jkNum
	}
	return jkBad
}

func (p *jsonIn) expectLit(lit string) error {
	p.skip()
	if !strings.HasPrefix(p.s[p.pos:], lit) {
		return p.fail("expected %s at offset %d", lit, p.pos)
	}
	p.pos += len(lit)
	return nil
}

func (p *jsonIn) expectCh(c byte) error {
	p.skip()
	if p.pos >= len(p.s) || p.s[p.pos] != c {
		return p.fail("expected '%c' at offset %d", c, p.pos)
	}
	p.pos++
	return nil
}

func isDigit(c byte) bool { return c >= '0' && c <= '9' }

// parseString parses a quoted string (single quotes accepted, legacy mode).
func (p *jsonIn) parseString() (string, error) {
	p.skip()
	if p.pos >= len(p.s) || (p.s[p.pos] != '"' && p.s[p.pos] != '\'') {
		return "", p.fail("expected a string at offset %d", p.pos)
	}
	quote := p.s[p.pos]
	p.pos++
	var sb strings.Builder
	for {
		if p.pos >= len(p.s) {
			return "", p.fail("truncated string")
		}
		c := p.s[p.pos]
		p.pos++
		if c == quote {
			out := sb.String()
			if !utf8.ValidString(out) {
				return "", p.fail("string is not valid UTF-8")
			}
			return out, nil
		}
		if c != '\\' {
			sb.WriteByte(c)
			continue
		}
		if p.pos >= len(p.s) {
			return "", p.fail("truncated escape")
		}
		e := p.s[p.pos]
		p.pos++
		switch e {
		case '"', '\\', '/', '\'':
			sb.WriteByte(e)
		case 'b':
			sb.WriteByte('\b')
		case 'f':
			sb.WriteByte('\f')
		case 'n':
			sb.WriteByte('\n')
		case 'r':
			sb.WriteByte('\r')
		case 't':
			sb.WriteByte('\t')
		case 'u', 'U':
			r, err := p.parseHex4()
			if err != nil {
				return "", err
			}
			if r >= 0xd800 && r <= 0xdbff {
				if p.pos+1 >= len(p.s) || p.s[p.pos] != '\\' || p.s[p.pos+1] != 'u' {
					return "", p.fail("unpaired surrogate escape")
				}
				p.pos += 2
				lo, err := p.parseHex4()
				if err != nil {
					return "", err
				}
				if lo < 0xdc00 || lo > 0xdfff {
					return "", p.fail("unpaired surrogate escape")
				}
				r = ((r & 0x3ff) << 10) | (lo & 0x3ff)
				r += 0x10000
			} else if r >= 0xdc00 && r <= 0xdfff {
				return "", p.fail("unpaired surrogate escape")
			}
			sb.WriteRune(rune(r))
		default:
			return "", p.fail("bad escape \\%c", e)
		}
	}
}

func (p *jsonIn) parseHex4() (uint32, error) {
	var v uint32
	for i := 0; i < 4; i++ {
		if p.pos >= len(p.s) {
			return 0, p.fail("truncated \\u escape")
		}
		h := p.s[p.pos]
		p.pos++
		v <<= 4
		switch {
		case h >= '0' && h <= '9':
			v |= uint32(h - '0')
		case h >= 'a' && h <= 'f':
			v |= uint32(h-'a') + 10
		case h >= 'A' && h <= 'F':
			v |= uint32(h-'A') + 10
		default:
			return 0, p.fail("bad \\u escape")
		}
	}
	return v, nil
}

// parseKey parses a quoted or bare (legacy) key.
func (p *jsonIn) parseKey() (string, error) {
	if p.peekKind() == jkStr {
		return p.parseString()
	}
	p.skip()
	start := p.pos
	for p.pos < len(p.s) {
		c := p.s[p.pos]
		if isDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' {
			p.pos++
		} else {
			break
		}
	}
	n := p.pos - start
	if n == 0 || isDigit(p.s[start]) {
		return "", p.fail("expected a field name at offset %d", start)
	}
	key := p.s[start:p.pos]
	if (n == 4 && (key == "true" || key == "null")) || (n == 5 && key == "false") {
		return "", p.fail("expected a field name at offset %d", start)
	}
	return key, nil
}

// parseRaw scans a number token without consuming what follows it.
func (p *jsonIn) parseRaw() (string, error) {
	p.skip()
	start := p.pos
	const (
		stInt = iota
		stFrac
		stExp
	)
	state := stInt
	var prev byte
	q := p.pos
	for q < len(p.s) {
		c := p.s[q]
		digit := isDigit(c)
		lastInt := isDigit(prev)
		switch {
		case digit:
		case state == stInt && c == '-':
			if lastInt {
				goto done
			}
		case state == stInt && lastInt && c == '.':
			state = stFrac
		case state != stExp && lastInt && (c == 'e' || c == 'E'):
			state = stExp
		case (prev == 'e' || prev == 'E') && (c == '-' || c == '+'):
		default:
			goto done
		}
		prev = c
		q++
	}
done:
	token := p.s[start:q]
	if len(token) == 0 || token == "-" {
		return "", p.fail("expected a number at offset %d", start)
	}
	i := 0
	if token[0] == '-' {
		i = 1
	}
	if len(token)-i > 1 && token[i] == '0' && isDigit(token[i+1]) {
		return "", p.fail("leading zero in %q", token)
	}
	if strings.HasSuffix(token, ".") {
		return "", p.fail("trailing '.' in %q", token)
	}
	p.pos = q
	p.skip()
	if p.pos < len(p.s) {
		switch p.s[p.pos] {
		case ',', ']', '}':
		default:
			return "", p.fail("number must end at a value boundary")
		}
	}
	return token, nil
}

// skipValue skips one JSON value (for unknown fields and Any scans).
func (p *jsonIn) skipValue() error {
	if err := p.enter(); err != nil {
		return err
	}
	defer func() { p.depth-- }()
	switch p.peekKind() {
	case jkObj:
		if err := p.expectCh('{'); err != nil {
			return err
		}
		p.skip()
		if p.pos < len(p.s) && p.s[p.pos] == '}' {
			p.pos++
			return nil
		}
		for {
			if _, err := p.parseKey(); err != nil {
				return err
			}
			if err := p.expectCh(':'); err != nil {
				return err
			}
			if err := p.skipValue(); err != nil {
				return err
			}
			p.skip()
			if p.pos < len(p.s) && p.s[p.pos] == ',' {
				p.pos++
				continue
			}
			return p.expectCh('}')
		}
	case jkArr:
		if err := p.expectCh('['); err != nil {
			return err
		}
		p.skip()
		if p.pos < len(p.s) && p.s[p.pos] == ']' {
			p.pos++
			return nil
		}
		for {
			if err := p.skipValue(); err != nil {
				return err
			}
			p.skip()
			if p.pos < len(p.s) && p.s[p.pos] == ',' {
				p.pos++
				p.skip()
				if p.pos < len(p.s) && p.s[p.pos] == ']' {
					p.pos++
					return nil
				}
				continue
			}
			return p.expectCh(']')
		}
	case jkStr:
		_, err := p.parseString()
		return err
	case jkNum:
		_, err := p.parseRaw()
		return err
	case jkTrue:
		return p.expectLit("true")
	case jkFalse:
		return p.expectLit("false")
	case jkNull:
		return p.expectLit("null")
	}
	return p.fail("expected a value at offset %d", p.pos)
}

// Strict [+-]?digits with 64-bit range checking.
func jsonAtoi64(text string) (int64, bool) {
	if len(text) == 0 {
		return 0, false
	}
	neg := false
	i := 0
	if text[0] == '-' || text[0] == '+' {
		neg = text[0] == '-'
		i = 1
	}
	if i >= len(text) {
		return 0, false
	}
	var acc uint64
	limit := uint64(math.MaxInt64)
	if neg {
		limit = uint64(math.MaxInt64) + 1
	}
	for ; i < len(text); i++ {
		c := text[i]
		if c < '0' || c > '9' {
			return 0, false
		}
		d := uint64(c - '0')
		if acc > (limit-d)/10 {
			return 0, false
		}
		acc = acc*10 + d
	}
	if neg {
		if acc == uint64(math.MaxInt64)+1 {
			return math.MinInt64, true
		}
		return -int64(acc), true
	}
	return int64(acc), true
}

func jsonAtou64(text string) (uint64, bool) {
	if len(text) == 0 {
		return 0, false
	}
	i := 0
	if text[0] == '+' {
		i = 1
	}
	if i >= len(text) {
		return 0, false
	}
	var acc uint64
	for ; i < len(text); i++ {
		c := text[i]
		if c < '0' || c > '9' {
			return 0, false
		}
		d := uint64(c - '0')
		if acc > (math.MaxUint64-d)/10 {
			return 0, false
		}
		acc = acc*10 + d
	}
	return acc, true
}

// jsonAtob is absl::SimpleAtob's word set, case-insensitive.
func jsonAtob(text string) (bool, bool) {
	if len(text) >= 8 || strings.IndexByte(text, 0) >= 0 {
		return false, false
	}
	switch strings.ToLower(text) {
	case "true", "1", "t", "y", "yes":
		return true, true
	case "false", "0", "f", "n", "no":
		return false, true
	}
	return false, false
}

func hasSpace(text string) bool {
	return strings.IndexAny(text, " \t\n\r") >= 0
}

// parseFloatFull is locale-free strtod with full-consumption checking.
func parseFloatFull(text string) (float64, bool) {
	if strings.IndexByte(text, 0) >= 0 || strings.Contains(text, "_") {
		return 0, false
	}
	v, err := strconv.ParseFloat(strings.TrimSpace(text), 64)
	if err != nil {
		return 0, false
	}
	return v, true
}

// parseIntValue parses an integer token or quoted string, allowing an
// integral float spelling (1e3, 100000.000) like the reference.
func parseIntValue(text string, quoted, unsigned, bits32 bool) (uint64, error) {
	if quoted && hasSpace(text) {
		return 0, fmt.Errorf("json: whitespace in %q", text)
	}
	if unsigned {
		if u, ok := jsonAtou64(text); ok {
			if bits32 && u > math.MaxUint32 {
				return 0, fmt.Errorf("json: uint32 out of range %q", text)
			}
			return u, nil
		}
		d, ok := parseFloatFull(text)
		if !ok || math.IsNaN(d) || math.IsInf(d, 0) || d < 0 || d > 18014398509481984.0 {
			return 0, fmt.Errorf("json: bad unsigned integer %q", text)
		}
		u := uint64(d)
		if float64(u) != d {
			return 0, fmt.Errorf("json: bad unsigned integer %q", text)
		}
		if bits32 && u > math.MaxUint32 {
			return 0, fmt.Errorf("json: uint32 out of range %q", text)
		}
		return u, nil
	}
	if s, ok := jsonAtoi64(text); ok {
		if bits32 && (s < math.MinInt32 || s > math.MaxInt32) {
			return 0, fmt.Errorf("json: int32 out of range %q", text)
		}
		return uint64(s), nil
	}
	d, ok := parseFloatFull(text)
	if !ok || math.IsNaN(d) || math.IsInf(d, 0) || d < -9007199254740992.0 || d > 9007199254740992.0 {
		return 0, fmt.Errorf("json: bad integer %q", text)
	}
	s := int64(d)
	if float64(s) != d {
		return 0, fmt.Errorf("json: bad integer %q", text)
	}
	if bits32 && (s < math.MinInt32 || s > math.MaxInt32) {
		return 0, fmt.Errorf("json: int32 out of range %q", text)
	}
	return uint64(s), nil
}

var errJSONInvalid = fmt.Errorf("json: invalid value")

// parseDoubleValue parses a double token or quoted string with the
// reference's NaN/Infinity spellings.
func parseDoubleValue(text string, quoted, isFloat bool) (float64, error) {
	switch text {
	case "NaN":
		return math.NaN(), nil
	case "Infinity":
		return math.Inf(1), nil
	case "-Infinity":
		return math.Inf(-1), nil
	}
	d, ok := parseFloatFull(text)
	if !ok {
		return 0, fmt.Errorf("json: bad double %q", text)
	}
	if !quoted && (math.IsNaN(d) || math.IsInf(d, 0)) {
		return 0, fmt.Errorf("json: bad double %q", text)
	}
	if isFloat && !math.IsNaN(d) && !math.IsInf(d, 0) && math.IsInf(float64(float32(d)), 0) {
		return 0, fmt.Errorf("json: float out of range %q", text)
	}
	return d, nil
}

func jsonBase64Value(c byte) int {
	switch {
	case c >= 'A' && c <= 'Z':
		return int(c - 'A')
	case c >= 'a' && c <= 'z':
		return int(c-'a') + 26
	case c >= '0' && c <= '9':
		return int(c-'0') + 52
	case c == '+' || c == '-':
		return 62
	case c == '/' || c == '_':
		return 63
	}
	return -1
}

// parseBase64 decodes with either alphabet and optional padding.
func parseBase64(text string) ([]byte, error) {
	n := len(text)
	for n > 0 && text[n-1] == '=' {
		n--
	}
	if len(text)-n > 2 {
		return nil, errJSONInvalid
	}
	if n == 0 {
		if len(text) == 0 {
			return nil, nil
		}
		return nil, errJSONInvalid
	}
	if len(text) == n {
		if n%4 == 1 {
			return nil, errJSONInvalid
		}
	} else if len(text)%4 != 0 {
		return nil, errJSONInvalid
	}
	out := make([]byte, 0, (n/4)*3+3)
	i := 0
	for ; i+4 <= n; i += 4 {
		a, b, c, d := jsonBase64Value(text[i]), jsonBase64Value(text[i+1]), jsonBase64Value(text[i+2]), jsonBase64Value(text[i+3])
		if a < 0 || b < 0 || c < 0 || d < 0 {
			return nil, errJSONInvalid
		}
		out = append(out, byte(a<<2|b>>4), byte(b&15<<4|c>>2), byte(c&3<<6|d))
	}
	switch n - i {
	case 2:
		a, b := jsonBase64Value(text[i]), jsonBase64Value(text[i+1])
		if a < 0 || b < 0 {
			return nil, errJSONInvalid
		}
		out = append(out, byte(a<<2|b>>4))
	case 3:
		a, b, c := jsonBase64Value(text[i]), jsonBase64Value(text[i+1]), jsonBase64Value(text[i+2])
		if a < 0 || b < 0 || c < 0 {
			return nil, errJSONInvalid
		}
		out = append(out, byte(a<<2|b>>4), byte(b&15<<4|c>>2))
	case 1:
		return nil, errJSONInvalid
	}
	return out, nil
}

// semanticUint converts stored bits to the Go value setters and oneof
// storage expect for an integer-family field.
func semanticUint(fp *FieldPlan, u uint64) any {
	switch fp.Proto {
	case descriptorpb.FieldDescriptorProto_TYPE_INT32,
		descriptorpb.FieldDescriptorProto_TYPE_SINT32,
		descriptorpb.FieldDescriptorProto_TYPE_SFIXED32,
		descriptorpb.FieldDescriptorProto_TYPE_ENUM:
		return int32(u)
	case descriptorpb.FieldDescriptorProto_TYPE_INT64,
		descriptorpb.FieldDescriptorProto_TYPE_SINT64,
		descriptorpb.FieldDescriptorProto_TYPE_SFIXED64:
		return int64(u)
	case descriptorpb.FieldDescriptorProto_TYPE_UINT32,
		descriptorpb.FieldDescriptorProto_TYPE_FIXED32:
		return uint32(u)
	case descriptorpb.FieldDescriptorProto_TYPE_BOOL:
		return u != 0
	case descriptorpb.FieldDescriptorProto_TYPE_FLOAT:
		return math.Float32frombits(uint32(u))
	case descriptorpb.FieldDescriptorProto_TYPE_DOUBLE:
		return math.Float64frombits(u)
	}
	return u
}

// storeUint stores an integer-family value; repeated fields append.
func storeUint(m *DMsg, ord int, fp *FieldPlan, u uint64) {
	s := &m.slots[ord]
	if fp.Repeated {
		s.present = true
		switch v := semanticUint(fp, u).(type) {
		case int32:
			appendRep(s, v)
		case int64:
			appendRep(s, v)
		case uint32:
			appendRep(s, v)
		case uint64:
			appendRep(s, v)
		case bool:
			appendRep(s, v)
		case float32:
			appendRep(s, v)
		case float64:
			appendRep(s, v)
		}
		return
	}
	switch fp.Proto {
	case descriptorpb.FieldDescriptorProto_TYPE_FLOAT:
		s.u = uint64(math.Float32bits(semanticUint(fp, u).(float32)))
	case descriptorpb.FieldDescriptorProto_TYPE_DOUBLE:
		s.u = math.Float64bits(semanticUint(fp, u).(float64))
	default:
		s.u = u
	}
	m.mark(ord, semanticUint(fp, u))
}

func storeString(m *DMsg, ord int, fp *FieldPlan, v string) {
	s := &m.slots[ord]
	if fp.Repeated {
		s.present = true
		appendRep(s, v)
		return
	}
	s.str = v
	m.mark(ord, v)
}

func storeBytes(m *DMsg, ord int, fp *FieldPlan, v []byte) {
	s := &m.slots[ord]
	if fp.Repeated {
		s.present = true
		appendRep(s, v)
		return
	}
	s.p = v
	m.mark(ord, v)
}

func storeMessage(m *DMsg, ord int, fp *FieldPlan, sub *DMsg) {
	if fp.Repeated {
		s := &m.slots[ord]
		s.present = true
		appendRep(s, sub)
		return
	}
	if fp.Presence == PresenceOneof {
		mergeOneof(m, ord, any(sub))
		return
	}
	s := &m.slots[ord]
	s.present = true
	s.m = sub
}

// storeUnknownEnum diverts an unknown closed-enum number to unknown fields.
func storeUnknownEnum(m *DMsg, fp *FieldPlan, num int32) {
	tag := uint64(fp.Number) << 3 // varint wire type
	m.unknown = AppendVarint(m.unknown, tag)
	m.unknown = AppendVarint(m.unknown, uint64(int64(num)))
}

func isValueField(m *DMsg, ord int, fp *FieldPlan) bool {
	return (fp.Proto == descriptorpb.FieldDescriptorProto_TYPE_MESSAGE || fp.Group) &&
		jsonClassify(fieldMsgFull(m, ord, fp)) == jwValue
}

func fieldMsgFull(m *DMsg, ord int, fp *FieldPlan) string {
	if fp.Proto != descriptorpb.FieldDescriptorProto_TYPE_MESSAGE && !fp.Group {
		return ""
	}
	sub, err := m.typ.sub(ord)
	if err != nil || sub == nil {
		return ""
	}
	return sub.plan.Full
}

func isNullValueField(fp *FieldPlan) bool {
	return fp.Proto == descriptorpb.FieldDescriptorProto_TYPE_ENUM && fp.Enum == jsonNullValue
}

// oneofSet reports whether another member of the same oneof is already set.
func oneofSet(m *DMsg, ord int, fp *FieldPlan) bool {
	for j, other := range m.typ.plan.Fields {
		if j != ord && other.Presence == PresenceOneof && other.Oneof == fp.Oneof && m.slots[j].present {
			return true
		}
	}
	return false
}

// lookupField resolves a key by camelCase, then proto name, then jsonName.
// The three namespaces each ride a memoized index (first wins, matching the
// linear scans); hand-built plans without indexes use the scans directly.
func lookupField(plan *MessagePlan, key string) (*FieldPlan, int) {
	if plan.byCamel != nil {
		if i, ok := plan.byCamel[key]; ok {
			return plan.Fields[i], i
		}
		if fp, ok := plan.byName[key]; ok {
			if j, ok := plan.ordByNum[fp.Number]; ok {
				return fp, j
			}
		}
		if i, ok := plan.byJSON[key]; ok {
			return plan.Fields[i], i
		}
		return nil, -1
	}
	for i, fp := range plan.Fields {
		if camelName(fp.Name) == key {
			return fp, i
		}
	}
	for i, fp := range plan.Fields {
		if fp.Name == key {
			return fp, i
		}
	}
	for i, fp := range plan.Fields {
		if jsonName(fp.Name) == key {
			return fp, i
		}
	}
	return nil, -1
}

func (p *jsonIn) parseScalarValue(m *DMsg, ord int, fp *FieldPlan) error {
	switch fp.Proto {
	case descriptorpb.FieldDescriptorProto_TYPE_DOUBLE,
		descriptorpb.FieldDescriptorProto_TYPE_FLOAT:
		isFloat := fp.Proto == descriptorpb.FieldDescriptorProto_TYPE_FLOAT
		switch p.peekKind() {
		case jkNum:
			text, err := p.parseRaw()
			if err != nil {
				return err
			}
			v, err := parseDoubleValue(text, false, isFloat)
			if err != nil {
				return err
			}
			if isFloat {
				storeUint(m, ord, fp, uint64(math.Float32bits(float32(v))))
			} else {
				storeUint(m, ord, fp, math.Float64bits(v))
			}
			return nil
		case jkStr:
			text, err := p.parseString()
			if err != nil {
				return err
			}
			v, err := parseDoubleValue(text, true, isFloat)
			if err != nil {
				return err
			}
			if isFloat {
				storeUint(m, ord, fp, uint64(math.Float32bits(float32(v))))
			} else {
				storeUint(m, ord, fp, math.Float64bits(v))
			}
			return nil
		}
		return p.fail("expected a number for field %q", fp.Name)
	case descriptorpb.FieldDescriptorProto_TYPE_INT64,
		descriptorpb.FieldDescriptorProto_TYPE_SINT64,
		descriptorpb.FieldDescriptorProto_TYPE_SFIXED64,
		descriptorpb.FieldDescriptorProto_TYPE_UINT64,
		descriptorpb.FieldDescriptorProto_TYPE_FIXED64,
		descriptorpb.FieldDescriptorProto_TYPE_INT32,
		descriptorpb.FieldDescriptorProto_TYPE_SINT32,
		descriptorpb.FieldDescriptorProto_TYPE_SFIXED32,
		descriptorpb.FieldDescriptorProto_TYPE_UINT32,
		descriptorpb.FieldDescriptorProto_TYPE_FIXED32:
		unsigned := fp.Proto == descriptorpb.FieldDescriptorProto_TYPE_UINT64 ||
			fp.Proto == descriptorpb.FieldDescriptorProto_TYPE_FIXED64 ||
			fp.Proto == descriptorpb.FieldDescriptorProto_TYPE_UINT32 ||
			fp.Proto == descriptorpb.FieldDescriptorProto_TYPE_FIXED32
		bits32 := fp.Proto == descriptorpb.FieldDescriptorProto_TYPE_INT32 ||
			fp.Proto == descriptorpb.FieldDescriptorProto_TYPE_SINT32 ||
			fp.Proto == descriptorpb.FieldDescriptorProto_TYPE_SFIXED32 ||
			fp.Proto == descriptorpb.FieldDescriptorProto_TYPE_UINT32 ||
			fp.Proto == descriptorpb.FieldDescriptorProto_TYPE_FIXED32
		switch p.peekKind() {
		case jkNum:
			text, err := p.parseRaw()
			if err != nil {
				return err
			}
			u, err := parseIntValue(text, false, unsigned, bits32)
			if err != nil {
				return err
			}
			storeUint(m, ord, fp, u)
			return nil
		case jkStr:
			text, err := p.parseString()
			if err != nil {
				return err
			}
			u, err := parseIntValue(text, true, unsigned, bits32)
			if err != nil {
				return err
			}
			storeUint(m, ord, fp, u)
			return nil
		}
		return p.fail("expected a number for field %q", fp.Name)
	case descriptorpb.FieldDescriptorProto_TYPE_BOOL:
		switch p.peekKind() {
		case jkTrue:
			if err := p.expectLit("true"); err != nil {
				return err
			}
			storeUint(m, ord, fp, 1)
			return nil
		case jkFalse:
			if err := p.expectLit("false"); err != nil {
				return err
			}
			storeUint(m, ord, fp, 0)
			return nil
		case jkStr:
			text, err := p.parseString()
			if err != nil {
				return err
			}
			v, ok := jsonAtob(text)
			if !ok {
				return p.fail("bad bool %q", text)
			}
			if v {
				storeUint(m, ord, fp, 1)
			} else {
				storeUint(m, ord, fp, 0)
			}
			return nil
		}
		return p.fail("expected a bool for field %q", fp.Name)
	case descriptorpb.FieldDescriptorProto_TYPE_STRING,
		descriptorpb.FieldDescriptorProto_TYPE_BYTES:
		if p.peekKind() != jkStr {
			return p.fail("expected a string for field %q", fp.Name)
		}
		text, err := p.parseString()
		if err != nil {
			return err
		}
		if fp.Proto == descriptorpb.FieldDescriptorProto_TYPE_STRING {
			storeString(m, ord, fp, text)
			return nil
		}
		raw, err := parseBase64(text)
		if err != nil {
			return p.fail("bad base64 for field %q", fp.Name)
		}
		storeBytes(m, ord, fp, raw)
		return nil
	}
	return p.fail("expected a scalar for field %q", fp.Name)
}

// parseEnumValue parses an enum value; skip is true when an unknown name was
// ignored and the caller must store nothing. Like src/json.c
// (pbw_enum_find_value) and Java GenericCodec.EnumDef.valueOf, every alias
// name resolves; print keeps the first-declared name from EnumPlan.Values.
func (p *jsonIn) parseEnumValue(m *DMsg, ord int, fp *FieldPlan) (bool, error) {
	ep, err := m.typ.s.Enum(fp.Enum)
	if err != nil {
		return false, p.fail("unknown enum %s", fp.Enum)
	}
	byName := ep.ByName
	if byName == nil {
		byName = map[string]int32{}
		for num, name := range ep.Values {
			byName[name] = num
		}
	}
	known := func(num int32) bool {
		_, ok := ep.Values[num]
		return ok
	}
	finish := func(num int32) (bool, error) {
		if known(num) {
			storeUint(m, ord, fp, uint64(int64(num)))
			return false, nil
		}
		if !ep.Closed {
			storeUint(m, ord, fp, uint64(int64(num)))
			return false, nil
		}
		storeUnknownEnum(m, fp, num)
		return false, nil
	}
	switch p.peekKind() {
	case jkStr:
		text, err := p.parseString()
		if err != nil {
			return false, err
		}
		if num, ok := byName[text]; ok {
			return finish(num)
		}
		trimmed := strings.Trim(text, " \t\n\r")
		if v, ok := jsonAtoi64(trimmed); ok && v >= math.MinInt32 && v <= math.MaxInt32 {
			return finish(int32(v))
		}
		if p.ignore {
			return true, nil
		}
		return false, p.fail("unknown enum name %q", text)
	case jkNum:
		text, err := p.parseRaw()
		if err != nil {
			return false, err
		}
		u, err := parseIntValue(text, false, false, true)
		if err != nil {
			return false, err
		}
		return finish(int32(u))
	case jkNull:
		if isNullValueField(fp) {
			if err := p.expectLit("null"); err != nil {
				return false, err
			}
			storeUint(m, ord, fp, 0)
			return false, nil
		}
	}
	return false, p.fail("expected an enum for field %q", fp.Name)
}

func (p *jsonIn) parseFieldValue(m *DMsg, ord int, fp *FieldPlan, inArray bool) error {
	// Any null is a no-op, except for Value and NullValue.
	if p.peekKind() == jkNull && !isValueField(m, ord, fp) && !isNullValueField(fp) {
		return p.expectLit("null")
	}
	if fp.Presence == PresenceOneof && oneofSet(m, ord, fp) {
		return p.fail("duplicate oneof member %q", fp.Name)
	}
	if fp.Proto == descriptorpb.FieldDescriptorProto_TYPE_MESSAGE || fp.Group {
		if fp.IsMap {
			return p.parseMap(m, ord, fp)
		}
		if fp.Repeated {
			if !inArray && p.peekKind() != jkArr {
				return p.fail("repeated field %q wants an array", fp.Name)
			}
			if !inArray {
				return p.parseArray(m, ord, fp)
			}
		}
		sub, isNew := existingOrNewSub(m, ord, fp)
		if !isNew {
			if err := p.parseMessageValue(sub); err != nil {
				return err
			}
			return nil
		}
		if err := p.parseMessageValue(sub); err != nil {
			return err
		}
		storeMessage(m, ord, fp, sub)
		return nil
	}
	if fp.Repeated {
		if !inArray && p.peekKind() != jkArr {
			return p.fail("repeated field %q wants an array", fp.Name)
		}
		if !inArray {
			return p.parseArray(m, ord, fp)
		}
		if fp.Proto == descriptorpb.FieldDescriptorProto_TYPE_ENUM {
			// The element arm stores its own value; only the map arm
			// needs the "ignored unknown name" flag.
			_, err := p.parseEnumValue(m, ord, fp)
			return err
		}
		return p.parseScalarValue(m, ord, fp)
	}
	if fp.Proto == descriptorpb.FieldDescriptorProto_TYPE_ENUM {
		_, err := p.parseEnumValue(m, ord, fp)
		return err
	}
	return p.parseScalarValue(m, ord, fp)
}

// existingOrNewSub returns the singular submessage to parse into: the
// already-present one when legacy duplicate keys merge, else a fresh tree.
func existingOrNewSub(m *DMsg, ord int, fp *FieldPlan) (*DMsg, bool) {
	if fp.Presence != PresenceOneof && m.slots[ord].present && m.slots[ord].m != nil {
		if want, err := m.typ.sub(ord); err == nil && want != nil &&
			m.slots[ord].m.typ.plan == want.plan {
			return m.slots[ord].m, false
		}
	}
	sub, err := m.typ.sub(ord)
	if err != nil || sub == nil {
		return nil, true
	}
	return sub.New(), true
}

// emitNull appends a repeated field's default element for a JSON null.
func (p *jsonIn) emitNull(m *DMsg, ord int, fp *FieldPlan) error {
	switch fp.Proto {
	case descriptorpb.FieldDescriptorProto_TYPE_STRING:
		storeString(m, ord, fp, "")
	case descriptorpb.FieldDescriptorProto_TYPE_BYTES:
		storeBytes(m, ord, fp, nil)
	case descriptorpb.FieldDescriptorProto_TYPE_MESSAGE:
		sub, err := m.typ.sub(ord)
		if err != nil || sub == nil {
			return p.fail("bad message field %q", fp.Name)
		}
		storeMessage(m, ord, fp, sub.New())
	default:
		if fp.Group {
			sub, err := m.typ.sub(ord)
			if err != nil || sub == nil {
				return p.fail("bad group field %q", fp.Name)
			}
			storeMessage(m, ord, fp, sub.New())
			return nil
		}
		storeUint(m, ord, fp, 0)
	}
	return nil
}

func (p *jsonIn) parseElement(m *DMsg, ord int, fp *FieldPlan) error {
	if p.peekKind() == jkNull {
		if isValueField(m, ord, fp) || isNullValueField(fp) {
			return p.parseFieldValue(m, ord, fp, true)
		}
		if err := p.expectLit("null"); err != nil {
			return err
		}
		return p.emitNull(m, ord, fp)
	}
	// Nested arrays flatten into the current one, except inside Value and
	// ListValue. The brackets still cost a level, like a plain array. One
	// type lookup decides both exceptions.
	if p.peekKind() == jkArr {
		if kind := jsonClassify(fieldMsgFull(m, ord, fp)); kind != jwValue && kind != jwList {
			if err := p.enter(); err != nil {
				return err
			}
			defer func() { p.depth-- }()
			if err := p.expectCh('['); err != nil {
				return err
			}
			p.skip()
			if p.pos < len(p.s) && p.s[p.pos] == ']' {
				p.pos++
				return nil
			}
			for {
				if err := p.parseElement(m, ord, fp); err != nil {
					return err
				}
				p.skip()
				if p.pos < len(p.s) && p.s[p.pos] == ',' {
					p.pos++
					p.skip()
					if p.pos < len(p.s) && p.s[p.pos] == ']' {
						p.pos++
						return nil
					}
					continue
				}
				return p.expectCh(']')
			}
		}
	}
	return p.parseFieldValue(m, ord, fp, true)
}

func (p *jsonIn) parseArray(m *DMsg, ord int, fp *FieldPlan) error {
	if err := p.enter(); err != nil {
		return err
	}
	defer func() { p.depth-- }()
	if err := p.expectCh('['); err != nil {
		return err
	}
	p.skip()
	if p.pos < len(p.s) && p.s[p.pos] == ']' {
		p.pos++
		return nil
	}
	for {
		if err := p.parseElement(m, ord, fp); err != nil {
			return err
		}
		p.skip()
		if p.pos < len(p.s) && p.s[p.pos] == ',' {
			p.pos++
			p.skip()
			if p.pos < len(p.s) && p.s[p.pos] == ']' {
				p.pos++
				return nil
			}
			continue
		}
		return p.expectCh(']')
	}
}

// parseMapKey stores one already-decoded object key into a map entry.
func (p *jsonIn) parseMapKey(text string, keyFP *FieldPlan, entry *DMsg, keyOrd int) error {
	switch keyFP.Proto {
	case descriptorpb.FieldDescriptorProto_TYPE_INT64,
		descriptorpb.FieldDescriptorProto_TYPE_SINT64,
		descriptorpb.FieldDescriptorProto_TYPE_SFIXED64,
		descriptorpb.FieldDescriptorProto_TYPE_INT32,
		descriptorpb.FieldDescriptorProto_TYPE_SINT32,
		descriptorpb.FieldDescriptorProto_TYPE_SFIXED32:
		trimmed := strings.Trim(text, " \t\n\r")
		v, ok := jsonAtoi64(trimmed)
		if !ok {
			return p.fail("bad map key %q", text)
		}
		if keyFP.Proto == descriptorpb.FieldDescriptorProto_TYPE_INT32 ||
			keyFP.Proto == descriptorpb.FieldDescriptorProto_TYPE_SINT32 ||
			keyFP.Proto == descriptorpb.FieldDescriptorProto_TYPE_SFIXED32 {
			if v < math.MinInt32 || v > math.MaxInt32 {
				return p.fail("bad map key %q", text)
			}
		}
		storeUint(entry, keyOrd, keyFP, uint64(v))
		return nil
	case descriptorpb.FieldDescriptorProto_TYPE_UINT64,
		descriptorpb.FieldDescriptorProto_TYPE_FIXED64,
		descriptorpb.FieldDescriptorProto_TYPE_UINT32,
		descriptorpb.FieldDescriptorProto_TYPE_FIXED32:
		trimmed := strings.Trim(text, " \t\n\r")
		v, ok := jsonAtou64(trimmed)
		if !ok {
			return p.fail("bad map key %q", text)
		}
		if keyFP.Proto == descriptorpb.FieldDescriptorProto_TYPE_UINT32 ||
			keyFP.Proto == descriptorpb.FieldDescriptorProto_TYPE_FIXED32 {
			if v > math.MaxUint32 {
				return p.fail("bad map key %q", text)
			}
		}
		storeUint(entry, keyOrd, keyFP, v)
		return nil
	case descriptorpb.FieldDescriptorProto_TYPE_BOOL:
		if text == "true" {
			storeUint(entry, keyOrd, keyFP, 1)
			return nil
		}
		if text == "false" {
			storeUint(entry, keyOrd, keyFP, 0)
			return nil
		}
		return p.fail("bad map key %q", text)
	case descriptorpb.FieldDescriptorProto_TYPE_STRING:
		storeString(entry, keyOrd, keyFP, text)
		return nil
	}
	return p.fail("bad map key %q", text)
}

func entryKeyValue(e *DMsg, keyOrd int, keyFP *FieldPlan) (uint64, string, bool) {
	if keyOrd < 0 || keyFP == nil {
		return 0, "", false
	}
	ks := &e.slots[keyOrd]
	if !ks.present {
		return 0, "", keyFP.Proto == descriptorpb.FieldDescriptorProto_TYPE_STRING
	}
	if keyFP.Proto == descriptorpb.FieldDescriptorProto_TYPE_STRING {
		return 0, ks.str, true
	}
	return ks.u, "", false
}

func checkDuplicateEntryKeys(items []*DMsg, keyOrd int, keyFP *FieldPlan) bool {
	seen := map[string]struct{}{}
	seenU := map[uint64]struct{}{}
	for _, e := range items {
		if e == nil {
			continue
		}
		u, s, isStr := entryKeyValue(e, keyOrd, keyFP)
		if isStr {
			if _, dup := seen[s]; dup {
				return true
			}
			seen[s] = struct{}{}
			continue
		}
		if keyOrd < 0 {
			continue
		}
		if _, dup := seenU[u]; dup {
			return true
		}
		seenU[u] = struct{}{}
	}
	return false
}

func (p *jsonIn) parseMap(m *DMsg, ord int, fp *FieldPlan) error {
	entryT, err := m.typ.sub(ord)
	if err != nil || entryT == nil {
		return p.fail("bad map field %q", fp.Name)
	}
	keyFP, keyOrd := fieldByNumber(entryT.plan, 1)
	valFP, valOrd := fieldByNumber(entryT.plan, 2)
	if keyFP == nil || valFP == nil {
		return p.fail("map entry %s has no key/value", entryT.plan.Full)
	}
	if err := p.expectCh('{'); err != nil {
		return err
	}
	p.skip()
	if p.pos < len(p.s) && p.s[p.pos] == '}' {
		p.pos++
		return nil
	}
	for {
		key, err := p.parseKey()
		if err != nil {
			return err
		}
		if err := p.expectCh(':'); err != nil {
			return err
		}
		item := entryT.New()
		if err := p.parseMapKey(key, keyFP, item, keyOrd); err != nil {
			return err
		}
		switch {
		case valFP.Proto == descriptorpb.FieldDescriptorProto_TYPE_ENUM:
			skip, err := p.parseEnumValue(item, valOrd, valFP)
			if err != nil {
				return err
			}
			if !skip {
				storeMessage(m, ord, fp, item)
			}
		case valFP.Proto == descriptorpb.FieldDescriptorProto_TYPE_MESSAGE || valFP.Group:
			valT, err := entryT.sub(valOrd)
			if err != nil || valT == nil {
				return p.fail("bad map value %q", fp.Name)
			}
			sub := valT.New()
			if err := p.parseMessageValue(sub); err != nil {
				return err
			}
			storeMessage(item, valOrd, valFP, sub)
			storeMessage(m, ord, fp, item)
		default:
			if err := p.parseScalarValue(item, valOrd, valFP); err != nil {
				return err
			}
			storeMessage(m, ord, fp, item)
		}
		p.skip()
		if p.pos < len(p.s) && p.s[p.pos] == ',' {
			p.pos++
			p.skip()
			if p.pos < len(p.s) && p.s[p.pos] == '}' {
				p.pos++
				goto done
			}
			continue
		}
		if err := p.expectCh('}'); err != nil {
			return err
		}
		goto done
	}
done:
	items, _ := m.slots[ord].rep.([]*DMsg)
	if checkDuplicateEntryKeys(items, keyOrd, keyFP) {
		return p.fail("duplicate map key in field %q", fp.Name)
	}
	return nil
}

func (p *jsonIn) parseNamedField(m *DMsg, key string) error {
	if len(key) >= 2 && key[0] == '[' && key[len(key)-1] == ']' {
		name := key[1 : len(key)-1]
		xp, ok := m.typ.s.Extension(name)
		if !ok || xp.Extendee != m.typ.plan.Full {
			if p.ignore {
				return p.skipValue()
			}
			return p.fail("unknown extension %q", key)
		}
		return p.parseExtension(m, xp)
	}
	fp, ord := lookupField(m.typ.plan, key)
	if fp == nil {
		if p.ignore {
			return p.skipValue()
		}
		return p.fail("unknown field %q", key)
	}
	return p.parseFieldValue(m, ord, fp, false)
}

// parseExtension parses one "[full.name]" value into the extension slot. The
// value rides the regular field parsers through a one-field holder message,
// so every scalar, repeated and message arm stays shared with regular fields.
func (p *jsonIn) parseExtension(m *DMsg, xp *ExtPlan) error {
	fp := xp.Field
	s := m.findExt(xp)
	if !fp.Repeated && s.present {
		return p.fail("duplicate extension %q", xp.Full)
	}
	holder := &DMsg{
		typ:   &DType{s: m.typ.s, plan: &MessagePlan{Full: m.typ.plan.Full, Fields: []*FieldPlan{fp}}},
		slots: make([]slot, 1),
	}
	if err := p.parseFieldValue(holder, 0, fp, false); err != nil {
		return err
	}
	tmp := holder.slots[0]
	if fp.Repeated {
		if tmp.rep != nil {
			s.present = true
			s.rep = appendRepeatedRep(s.rep, tmp.rep)
		}
		return nil
	}
	*s = tmp
	return nil
}

func (p *jsonIn) parseObjectBody(m *DMsg) error {
	p.skip()
	if p.pos < len(p.s) && p.s[p.pos] == '}' {
		p.pos++
		return nil
	}
	for {
		key, err := p.parseKey()
		if err != nil {
			return err
		}
		if err := p.expectCh(':'); err != nil {
			return err
		}
		if err := p.parseNamedField(m, key); err != nil {
			return err
		}
		p.skip()
		if p.pos < len(p.s) && p.s[p.pos] == ',' {
			p.pos++
			p.skip()
			if p.pos < len(p.s) && p.s[p.pos] == '}' {
				p.pos++
				return nil
			}
			continue
		}
		return p.expectCh('}')
	}
}

// parseMessageValue routes a message value by kind, like the reference.
func (p *jsonIn) parseMessageValue(m *DMsg) error {
	if err := p.enter(); err != nil {
		return err
	}
	defer func() { p.depth-- }()
	switch jsonClassify(m.typ.plan.Full) {
	case jwAny:
		if p.peekKind() == jkNull {
			return p.expectLit("null")
		}
		return p.parseAny(m)
	case jwValue:
		return p.parseValueMsg(m)
	case jwStruct:
		return p.parseStruct(m)
	}
	if p.peekKind() != jkObj {
		switch jsonClassify(m.typ.plan.Full) {
		case jwList:
			return p.parseListOf(m)
		case jwWrapper:
			fp, ord := fieldByNumber(m.typ.plan, 1)
			if fp == nil {
				return p.fail("wrapper has no value field")
			}
			return p.parseFieldValue(m, ord, fp, false)
		case jwTimestamp:
			return p.parseTimestamp(m)
		case jwDuration:
			return p.parseDuration(m)
		case jwFieldMask:
			return p.parseFieldMask(m)
		case jwEmpty:
			return p.fail("expected an object for google.protobuf.Empty")
		default:
			return p.fail("expected an object at offset %d", p.pos)
		}
	}
	if err := p.expectCh('{'); err != nil {
		return err
	}
	return p.parseObjectBody(m)
}

func timeField(plan *MessagePlan, num uint32, proto descriptorpb.FieldDescriptorProto_Type) (*FieldPlan, int) {
	fp, ord := fieldByNumber(plan, num)
	if fp == nil || fp.Repeated || fp.Proto != proto {
		return nil, -1
	}
	return fp, ord
}

func (p *jsonIn) parseTimestamp(m *DMsg) error {
	secsFP, secsOrd := timeField(m.typ.plan, 1, descriptorpb.FieldDescriptorProto_TYPE_INT64)
	nanosFP, nanosOrd := timeField(m.typ.plan, 2, descriptorpb.FieldDescriptorProto_TYPE_INT32)
	if secsFP == nil || nanosFP == nil {
		return p.fail("bad Timestamp shape")
	}
	if p.peekKind() == jkNull {
		return p.expectLit("null")
	}
	text, err := p.parseString()
	if err != nil {
		return err
	}
	// Fixed offsets, exactly like the reference: YYYY-MM-DDTHH:MM:SS,
	// optional .fraction (1-9 digits), then Z or +hh:mm.
	if len(text) < 20 {
		return p.fail("bad Timestamp %q", text)
	}
	parts := [6]string{text[0:4], text[5:7], text[8:10], text[11:13], text[14:16], text[17:19]}
	if text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' || text[16] != ':' {
		return p.fail("bad Timestamp %q", text)
	}
	nums := [6]int{}
	for i, part := range parts {
		for j := 0; j < len(part); j++ {
			if !isDigit(part[j]) {
				return p.fail("bad Timestamp %q", text)
			}
			nums[i] = nums[i]*10 + int(part[j]-'0')
		}
	}
	year, mon, day, hour, min, sec := nums[0], nums[1], nums[2], nums[3], nums[4], nums[5]
	if year == 0 || mon == 0 || mon > 12 || day == 0 || hour > 23 || min > 59 || sec > 59 {
		return p.fail("bad Timestamp %q", text)
	}
	if day > daysInMonth(year, mon) {
		return p.fail("bad Timestamp %q", text)
	}
	rest := text[19:]
	var nanos int
	if strings.HasPrefix(rest, ".") {
		rest = rest[1:]
		digits := 0
		frac := 0
		for len(rest) > 0 && isDigit(rest[0]) {
			if digits >= 9 {
				return p.fail("bad Timestamp %q", text)
			}
			frac = frac*10 + int(rest[0]-'0')
			digits++
			rest = rest[1:]
		}
		if digits == 0 {
			return p.fail("bad Timestamp %q", text)
		}
		for ; digits < 9; digits++ {
			frac *= 10
		}
		nanos = frac
	}
	var secs int64
	switch {
	case rest == "Z":
	case len(rest) == 6 && (rest[0] == '+' || rest[0] == '-') && rest[3] == ':' &&
		isDigit(rest[1]) && isDigit(rest[2]) && isDigit(rest[4]) && isDigit(rest[5]):
		offHour := int(rest[1]-'0')*10 + int(rest[2]-'0')
		offMin := int(rest[4]-'0')*10 + int(rest[5]-'0')
		off := int64(offHour*60+offMin) * 60
		base := time.Date(year, time.Month(mon), day, hour, min, sec, 0, time.UTC).Unix()
		if rest[0] == '-' {
			secs = base + off
		} else {
			secs = base - off
		}
		rest = ""
	default:
		return p.fail("bad Timestamp %q", text)
	}
	if rest == "Z" {
		secs = time.Date(year, time.Month(mon), day, hour, min, sec, 0, time.UTC).Unix()
	}
	if secs < -62135596800 || secs > 253402300799 {
		return p.fail("Timestamp out of range %q", text)
	}
	storeUint(m, secsOrd, secsFP, uint64(secs))
	storeUint(m, nanosOrd, nanosFP, uint64(nanos))
	return nil
}

func daysInMonth(year, mon int) int {
	switch mon {
	case 1, 3, 5, 7, 8, 10, 12:
		return 31
	case 4, 6, 9, 11:
		return 30
	case 2:
		if year%4 == 0 && (year%100 != 0 || year%400 == 0) {
			return 29
		}
		return 28
	}
	return 0
}

func (p *jsonIn) parseDuration(m *DMsg) error {
	secsFP, secsOrd := timeField(m.typ.plan, 1, descriptorpb.FieldDescriptorProto_TYPE_INT64)
	nanosFP, nanosOrd := timeField(m.typ.plan, 2, descriptorpb.FieldDescriptorProto_TYPE_INT32)
	if secsFP == nil || nanosFP == nil {
		return p.fail("bad Duration shape")
	}
	if p.peekKind() == jkNull {
		return p.expectLit("null")
	}
	text, err := p.parseString()
	if err != nil {
		return err
	}
	intEnd := 0
	for intEnd < len(text) && (isDigit(text[intEnd]) || text[intEnd] == '-') {
		intEnd++
	}
	secs, ok := jsonAtoi64(text[:intEnd])
	if intEnd == 0 || !ok {
		return p.fail("bad Duration %q", text)
	}
	const max = int64(3652500) * 86400
	if secs > max || secs < -max {
		return p.fail("Duration out of range %q", text)
	}
	rest := text[intEnd:]
	nanos := 0
	if strings.HasPrefix(rest, ".") {
		rest = rest[1:]
		digits, frac := 0, 0
		for len(rest) > 0 && isDigit(rest[0]) {
			if digits >= 9 {
				return p.fail("bad Duration %q", text)
			}
			frac = frac*10 + int(rest[0]-'0')
			digits++
			rest = rest[1:]
		}
		if digits == 0 {
			return p.fail("bad Duration %q", text)
		}
		for ; digits < 9; digits++ {
			frac *= 10
		}
		nanos = frac
	}
	if len(text) > 0 && text[0] == '-' || secs < 0 {
		nanos = -nanos
	}
	if len(rest) != 1 || rest[0] != 's' {
		return p.fail("bad Duration %q", text)
	}
	storeUint(m, secsOrd, secsFP, uint64(secs))
	storeUint(m, nanosOrd, nanosFP, uint64(int64(int32(nanos))))
	return nil
}

func (p *jsonIn) parseFieldMask(m *DMsg) error {
	if p.peekKind() == jkNull {
		return p.expectLit("null")
	}
	text, err := p.parseString()
	if err != nil {
		return err
	}
	pathsFP, pathsOrd := fieldByNumber(m.typ.plan, 1)
	if pathsFP == nil {
		return p.fail("bad FieldMask shape")
	}
	if len(text) == 0 {
		return nil
	}
	for _, path := range strings.Split(text, ",") {
		var sb strings.Builder
		for i := 0; i < len(path); i++ {
			c := path[i]
			if c >= 'A' && c <= 'Z' {
				sb.WriteByte('_')
				sb.WriteByte(c - 'A' + 'a')
			} else {
				sb.WriteByte(c)
			}
		}
		storeString(m, pathsOrd, pathsFP, sb.String())
	}
	return nil
}

func (p *jsonIn) parseValueMsg(m *DMsg) error {
	f := [6]int{}
	fp := [6]*FieldPlan{}
	for n := uint32(1); n <= 6; n++ {
		got, ord := fieldByNumber(m.typ.plan, n)
		if got == nil {
			return p.fail("bad Value shape")
		}
		fp[n-1], f[n-1] = got, ord
	}
	switch p.peekKind() {
	case jkNull:
		if err := p.expectLit("null"); err != nil {
			return err
		}
		storeUint(m, f[0], fp[0], 0)
		return nil
	case jkNum:
		text, err := p.parseRaw()
		if err != nil {
			return err
		}
		d, ok := parseFloatFull(text)
		if !ok || math.IsNaN(d) || math.IsInf(d, 0) {
			return p.fail("bad Value number %q", text)
		}
		storeUint(m, f[1], fp[1], math.Float64bits(d))
		return nil
	case jkStr:
		text, err := p.parseString()
		if err != nil {
			return err
		}
		storeString(m, f[2], fp[2], text)
		return nil
	case jkTrue:
		if err := p.expectLit("true"); err != nil {
			return err
		}
		storeUint(m, f[3], fp[3], 1)
		return nil
	case jkFalse:
		if err := p.expectLit("false"); err != nil {
			return err
		}
		storeUint(m, f[3], fp[3], 0)
		return nil
	case jkObj, jkArr:
		n := 4
		if p.peekKind() == jkArr {
			n = 5
		}
		subT, err := m.typ.sub(f[n])
		if err != nil || subT == nil {
			return p.fail("bad Value shape")
		}
		sub := subT.New()
		if err := p.parseMessageValue(sub); err != nil {
			return err
		}
		storeMessage(m, f[n], fp[n], sub)
		return nil
	}
	return p.fail("expected a Value at offset %d", p.pos)
}

func (p *jsonIn) parseStruct(m *DMsg) error {
	entryFP, entryOrd := fieldByNumber(m.typ.plan, 1)
	if entryFP == nil {
		return p.fail("bad Struct shape")
	}
	entryT, err := m.typ.sub(entryOrd)
	if err != nil || entryT == nil {
		return p.fail("bad Struct shape")
	}
	keyFP, keyOrd := fieldByNumber(entryT.plan, 1)
	valFP, valOrd := fieldByNumber(entryT.plan, 2)
	if keyFP == nil || valFP == nil {
		return p.fail("bad Struct shape")
	}
	if p.peekKind() == jkNull {
		return p.expectLit("null")
	}
	if err := p.expectCh('{'); err != nil {
		return err
	}
	p.skip()
	if p.pos < len(p.s) && p.s[p.pos] == '}' {
		p.pos++
		return nil
	}
	for {
		key, err := p.parseKey()
		if err != nil {
			return err
		}
		if err := p.expectCh(':'); err != nil {
			return err
		}
		item := entryT.New()
		storeString(item, keyOrd, keyFP, key)
		valT, err := entryT.sub(valOrd)
		if err != nil || valT == nil {
			return p.fail("bad Struct shape")
		}
		val := valT.New()
		if err := p.parseMessageValue(val); err != nil {
			return err
		}
		storeMessage(item, valOrd, valFP, val)
		storeMessage(m, entryOrd, entryFP, item)
		p.skip()
		if p.pos < len(p.s) && p.s[p.pos] == ',' {
			p.pos++
			p.skip()
			if p.pos < len(p.s) && p.s[p.pos] == '}' {
				p.pos++
				break
			}
			continue
		}
		if err := p.expectCh('}'); err != nil {
			return err
		}
		break
	}
	items, _ := m.slots[entryOrd].rep.([]*DMsg)
	seen := map[string]struct{}{}
	for _, e := range items {
		if e == nil {
			continue
		}
		k := e.slots[keyOrd].str
		if _, dup := seen[k]; dup {
			return p.fail("duplicate Struct key %q", k)
		}
		seen[k] = struct{}{}
	}
	return nil
}

func (p *jsonIn) parseListOf(m *DMsg) error {
	entryFP, entryOrd := fieldByNumber(m.typ.plan, 1)
	if entryFP == nil {
		return p.fail("bad ListValue shape")
	}
	entryT, err := m.typ.sub(entryOrd)
	if err != nil || entryT == nil {
		return p.fail("bad ListValue shape")
	}
	if p.peekKind() == jkNull {
		return p.expectLit("null")
	}
	if err := p.expectCh('['); err != nil {
		return err
	}
	p.skip()
	if p.pos < len(p.s) && p.s[p.pos] == ']' {
		p.pos++
		return nil
	}
	for {
		item := entryT.New()
		if err := p.parseValueMsg(item); err != nil {
			return err
		}
		storeMessage(m, entryOrd, entryFP, item)
		p.skip()
		if p.pos < len(p.s) && p.s[p.pos] == ',' {
			p.pos++
			p.skip()
			if p.pos < len(p.s) && p.s[p.pos] == ']' {
				p.pos++
				return nil
			}
			continue
		}
		return p.expectCh(']')
	}
}

// parseAny buffers one object, resolves @type, then parses the expansion.
func (p *jsonIn) parseAny(m *DMsg) error {
	urlFP, urlOrd := timeField(m.typ.plan, 1, descriptorpb.FieldDescriptorProto_TYPE_STRING)
	valFP, valOrd := timeField(m.typ.plan, 2, descriptorpb.FieldDescriptorProto_TYPE_BYTES)
	if urlFP == nil || valFP == nil {
		return p.fail("bad Any shape")
	}
	if err := p.expectCh('{'); err != nil {
		return err
	}
	p.skip()
	if p.pos < len(p.s) && p.s[p.pos] == '}' {
		p.pos++
		return nil
	}
	type span struct {
		key        string
		text       string
		hasTypeURL bool
	}
	var fields []span
	var url string
	for {
		key, err := p.parseKey()
		if err != nil {
			return err
		}
		if err := p.expectCh(':'); err != nil {
			return err
		}
		if key == "@type" {
			if url != "" {
				return p.fail("duplicate @type")
			}
			u, err := p.parseString()
			if err != nil {
				return err
			}
			url = u
			fields = append(fields, span{key: key, hasTypeURL: true})
		} else {
			start := p.pos
			if err := p.skipValue(); err != nil {
				return err
			}
			fields = append(fields, span{key: key, text: p.s[start:p.pos]})
		}
		p.skip()
		if p.pos < len(p.s) && p.s[p.pos] == ',' {
			p.pos++
			p.skip()
			if p.pos < len(p.s) && p.s[p.pos] == '}' {
				p.pos++
				break
			}
			continue
		}
		if err := p.expectCh('}'); err != nil {
			return err
		}
		break
	}
	if url == "" {
		return p.fail("Any object has no @type")
	}
	name := url
	if k := strings.LastIndexByte(url, '/'); k >= 0 {
		name = url[k+1:]
	}
	sub, err := m.typ.s.Type(name)
	if err != nil {
		return p.fail("Any type is not resolvable: %s", url)
	}
	inner := sub.New()
	kind := jsonClassify(sub.plan.Full)
	for _, f := range fields {
		if f.hasTypeURL {
			continue
		}
		q := &jsonIn{s: f.text, ignore: p.ignore, lim: p.lim, depth: p.depth}
		if kind != jwNone {
			if f.key != "value" {
				return p.fail("Any %s wants a \"value\" key", name)
			}
			if err := q.parseMessageValue(inner); err != nil {
				return err
			}
			continue
		}
		fp, ord := lookupField(inner.typ.plan, f.key)
		if fp == nil {
			if p.ignore {
				continue
			}
			return p.fail("unknown field %q", f.key)
		}
		if err := q.parseFieldValue(inner, ord, fp, false); err != nil {
			return err
		}
		q.skip()
		if q.pos != len(q.s) {
			return p.fail("trailing bytes in field %q", f.key)
		}
	}
	payload, err := inner.Encode()
	if err != nil {
		return err
	}
	storeString(m, urlOrd, urlFP, url)
	if len(payload) > 0 {
		storeBytes(m, valOrd, valFP, payload)
	}
	return nil
}

// jCheckRequired rejects trees missing a proto2 required field, recursing
// into present submessages like the binary decoder does.
func jCheckRequired(m *DMsg) error {
	for i, fp := range m.typ.plan.Fields {
		if fp.Presence == PresenceRequired && !m.slots[i].present {
			return fmt.Errorf("json: missing required field %q", fp.Name)
		}
	}
	for i, fp := range m.typ.plan.Fields {
		if fp.Proto != descriptorpb.FieldDescriptorProto_TYPE_MESSAGE && !fp.Group {
			continue
		}
		if fp.Repeated {
			subs, _ := m.slots[i].rep.([]*DMsg)
			for _, sub := range subs {
				if sub != nil {
					if err := jCheckRequired(sub); err != nil {
						return err
					}
				}
			}
			continue
		}
		if sub := m.slots[i].m; sub != nil {
			if err := jCheckRequired(sub); err != nil {
				return err
			}
		}
	}
	return nil
}
