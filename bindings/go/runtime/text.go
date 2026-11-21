// Text format over the dynamic core: prints the multi-line form the
// reference implementation produces and parses what its parser accepts.
// Mirrors src/text.c, which is the authority for every semantic here, and
// bindings/java/runtime/Text.java.
//
// Scalars print as `name: value`, messages as `name { ... }` (two-space
// indent, colon optional on input), groups under their unqualified type
// name, extensions would print as `[full.name]` (the Go plans carry no
// extensions, so brackets never resolve), and every repeated element prints
// on its own line. Unknown fields print as numeric lines only when the
// caller asks (they never round-trip). The parser accepts optional colons
// before `{`, `<>` delimiters, the `[a, b]` list form for repeated fields,
// group type names, decimal/hex/octal integers, C escapes with adjacent
// string concatenation, and `,`/`;` separators. Unknown field names fail
// unless reserved, which the parser skips silently like the reference.
// Unlike the binary format, a singular field or oneof member may only be
// named once: text format has no last-wins and no message merge.
package runtime

import (
	"fmt"
	"math"
	"strconv"
	"strings"
	"unicode/utf8"

	"google.golang.org/protobuf/types/descriptorpb"
)

// MarshalText prints a message in text format; unknown fields print as
// numeric lines only when printUnknown is set.
func (t *DType) MarshalText(m *DMsg, printUnknown bool) (string, error) {
	o := &textOut{printUnknown: printUnknown}
	o.printMessage(m, 0)
	if o.err != nil {
		return "", o.err
	}
	return o.sb.String(), nil
}

// UnmarshalText parses text format into a new tree. Unknown field names
// fail unless the schema reserved them, a repeated singular or oneof member
// fails, and a missing proto2 required field fails after the parse.
func (t *DType) UnmarshalText(text string) (*DMsg, error) {
	return t.UnmarshalTextLimits(text, Limits{})
}

// UnmarshalTextLimits is UnmarshalText with explicit bounds: documents
// larger than the byte cap fail before anything parses, and nesting past
// the effective cap fails. Like the reference, a list costs a level too.
// The old name keeps its signature as a defaults wrapper.
func (t *DType) UnmarshalTextLimits(text string, lim Limits) (*DMsg, error) {
	if err := lim.checkBytes(len(text)); err != nil {
		return nil, err
	}
	m := t.New()
	p := &textIn{s: text, lim: lim}
	if err := p.parseBody(m); err != nil {
		return nil, err
	}
	if err := tCheckRequired(m); err != nil {
		return nil, err
	}
	return m, nil
}

// shortName drops the package prefix: groups print under their type name.
func shortName(full string) string {
	if i := strings.LastIndexByte(full, '.'); i >= 0 {
		return full[i+1:]
	}
	return full
}

// textShouldPrint mirrors the reference printer: absent fields never print,
// empty repeated fields never print, implicit-presence scalars skip their
// zero value, and everything else (including empty messages) prints.
func textShouldPrint(m *DMsg, i int, fp *FieldPlan) bool {
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

// textEnumName resolves an enum value to its first-declared name.
func textEnumName(t *DType, fp *FieldPlan, num int32) (string, bool) {
	ep, err := t.s.Enum(fp.Enum)
	if err != nil {
		return "", false
	}
	name, ok := ep.Values[num]
	return name, ok
}

/* ------------------------------------------------------------ printer */

type textOut struct {
	sb           strings.Builder
	indent       int
	printUnknown bool
	err          error
}

func (o *textOut) fail(err error) {
	if o.err == nil {
		o.err = err
	}
}

func (o *textOut) str(s string) {
	if o.err == nil {
		o.sb.WriteString(s)
	}
}

func (o *textOut) pad() {
	for i := 0; i < o.indent; i++ {
		o.str("  ")
	}
}

func (o *textOut) printMessage(m *DMsg, depth int) {
	if o.err != nil || m == nil || m.typ == nil {
		o.fail(fmt.Errorf("text: cannot print a nil message"))
		return
	}
	for i, fp := range m.typ.plan.Fields {
		if !textShouldPrint(m, i, fp) {
			continue
		}
		if fp.Repeated {
			for k := 0; k < repLen(&m.slots[i]); k++ {
				o.printField(m, i, fp, k)
			}
			continue
		}
		o.printField(m, i, fp, -1)
	}
	if o.printUnknown && len(m.unknown) > 0 {
		o.printUnknowns(m.unknown, depth)
	}
}

// displayName is the printed field name: groups use the short type name.
func displayName(fp *FieldPlan) string {
	if fp.Group {
		return shortName(fp.Message)
	}
	return fp.Name
}

func (o *textOut) printField(m *DMsg, i int, fp *FieldPlan, elem int) {
	if o.err != nil {
		return
	}
	s := &m.slots[i]
	o.pad()
	o.str(displayName(fp))
	if fp.Proto == descriptorpb.FieldDescriptorProto_TYPE_MESSAGE || fp.Group {
		var sub *DMsg
		if elem >= 0 {
			items, _ := s.rep.([]*DMsg)
			if elem < len(items) {
				sub = items[elem]
			}
		} else {
			sub = s.m
		}
		if sub == nil {
			o.fail(fmt.Errorf("text: field %q holds a nil message", fp.Name))
			return
		}
		o.str(" {\n")
		o.indent++
		o.printMessage(sub, 0)
		o.indent--
		o.pad()
		o.str("}\n")
		return
	}
	o.str(": ")
	o.printScalar(m.typ, fp, s, elem)
	o.str("\n")
}

func (o *textOut) printScalar(t *DType, fp *FieldPlan, s *slot, elem int) {
	u, str, p := s.u, s.str, s.p
	if elem >= 0 {
		switch fp.Proto {
		case descriptorpb.FieldDescriptorProto_TYPE_INT32,
			descriptorpb.FieldDescriptorProto_TYPE_SINT32,
			descriptorpb.FieldDescriptorProto_TYPE_SFIXED32,
			descriptorpb.FieldDescriptorProto_TYPE_ENUM:
			u = uint64(int64(repOf[int32](s)[elem]))
		case descriptorpb.FieldDescriptorProto_TYPE_INT64,
			descriptorpb.FieldDescriptorProto_TYPE_SINT64,
			descriptorpb.FieldDescriptorProto_TYPE_SFIXED64:
			u = uint64(repOf[int64](s)[elem])
		case descriptorpb.FieldDescriptorProto_TYPE_UINT32,
			descriptorpb.FieldDescriptorProto_TYPE_FIXED32:
			u = uint64(repOf[uint32](s)[elem])
		case descriptorpb.FieldDescriptorProto_TYPE_UINT64,
			descriptorpb.FieldDescriptorProto_TYPE_FIXED64:
			u = repOf[uint64](s)[elem]
		case descriptorpb.FieldDescriptorProto_TYPE_FLOAT:
			u = uint64(math.Float32bits(repOf[float32](s)[elem]))
		case descriptorpb.FieldDescriptorProto_TYPE_DOUBLE:
			u = math.Float64bits(repOf[float64](s)[elem])
		case descriptorpb.FieldDescriptorProto_TYPE_BOOL:
			if repOf[bool](s)[elem] {
				u = 1
			} else {
				u = 0
			}
		case descriptorpb.FieldDescriptorProto_TYPE_STRING:
			str = repOf[string](s)[elem]
		case descriptorpb.FieldDescriptorProto_TYPE_BYTES:
			p = repOf[[]byte](s)[elem]
		}
	}
	switch fp.Proto {
	case descriptorpb.FieldDescriptorProto_TYPE_DOUBLE:
		printTextDouble(o, math.Float64frombits(u), false)
	case descriptorpb.FieldDescriptorProto_TYPE_FLOAT:
		printTextDouble(o, float64(math.Float32frombits(uint32(u))), true)
	case descriptorpb.FieldDescriptorProto_TYPE_BOOL:
		if u != 0 {
			o.str("true")
		} else {
			o.str("false")
		}
	case descriptorpb.FieldDescriptorProto_TYPE_ENUM:
		if name, ok := textEnumName(t, fp, int32(u)); ok {
			o.str(name)
			return
		}
		o.str(strconv.FormatInt(int64(int32(u)), 10))
	case descriptorpb.FieldDescriptorProto_TYPE_STRING:
		quoteTextString(o, []byte(str))
	case descriptorpb.FieldDescriptorProto_TYPE_BYTES:
		quoteTextString(o, p)
	case descriptorpb.FieldDescriptorProto_TYPE_INT32,
		descriptorpb.FieldDescriptorProto_TYPE_SINT32,
		descriptorpb.FieldDescriptorProto_TYPE_SFIXED32:
		o.str(strconv.FormatInt(int64(int32(u)), 10))
	case descriptorpb.FieldDescriptorProto_TYPE_INT64,
		descriptorpb.FieldDescriptorProto_TYPE_SINT64,
		descriptorpb.FieldDescriptorProto_TYPE_SFIXED64:
		o.str(strconv.FormatInt(int64(u), 10))
	case descriptorpb.FieldDescriptorProto_TYPE_UINT32,
		descriptorpb.FieldDescriptorProto_TYPE_FIXED32:
		o.str(strconv.FormatUint(uint64(uint32(u)), 10))
	default: // uint64, fixed64
		o.str(strconv.FormatUint(u, 10))
	}
}

// printTextDouble is the C t_double loop: the shortest %g spelling from the
// minimum precision up that still parses back to the same bits.
func printTextDouble(o *textOut, v float64, isFloat bool) {
	if math.IsNaN(v) {
		o.str("nan")
		return
	}
	if math.IsInf(v, 1) {
		o.str("inf")
		return
	}
	if math.IsInf(v, -1) {
		o.str("-inf")
		return
	}
	precMin := 15
	if isFloat {
		precMin = 9
	}
	for prec := precMin; prec <= 17; prec++ {
		s := strconv.FormatFloat(v, 'g', prec, 64)
		back, err := strconv.ParseFloat(s, 64)
		if err != nil {
			continue
		}
		if isFloat {
			if float64(float32(back)) == v {
				o.str(s)
				return
			}
			continue
		}
		if back == v {
			o.str(s)
			return
		}
	}
	o.str("0")
}

// quoteTextString is absl::CEscape: short escapes only for tab/lf/cr,
// escaped quotes, three-digit octal for the rest.
func quoteTextString(o *textOut, b []byte) {
	const oct = "01234567"
	o.str("\"")
	for _, c := range b {
		switch c {
		case '\t':
			o.str("\\t")
		case '\n':
			o.str("\\n")
		case '\r':
			o.str("\\r")
		case '"':
			o.str("\\\"")
		case '\\':
			o.str("\\\\")
		case '\'':
			o.str("\\'")
		default:
			if c >= 32 && c < 127 {
				o.sb.WriteByte(c)
				continue
			}
			o.str("\\")
			o.sb.WriteByte(oct[c>>6])
			o.sb.WriteByte(oct[(c>>3)&7])
			o.sb.WriteByte(oct[c&7])
		}
	}
	o.str("\"")
}

/* --------------------------------------------------- unknown printer */

func (o *textOut) printUnknowns(raw []byte, depth int) {
	for len(raw) > 0 && o.err == nil {
		num, wt, used, err := ConsumeTag(raw)
		if err != nil {
			o.fail(fmt.Errorf("text: bad unknown field: %w", err))
			return
		}
		raw = raw[used:]
		raw = o.printUnknownValue(num, wt, raw, depth)
	}
}

func (o *textOut) printUnknownValue(num uint32, wt WireType, raw []byte, depth int) []byte {
	o.pad()
	o.str(strconv.FormatUint(uint64(num), 10))
	switch wt {
	case WireVarint:
		v, used, err := ConsumeVarint(raw)
		if err != nil {
			o.fail(fmt.Errorf("text: bad unknown varint: %w", err))
			return nil
		}
		o.str(": ")
		o.str(strconv.FormatUint(v, 10))
		o.str("\n")
		return raw[used:]
	case WireFixed64:
		v, used, err := ConsumeFixed64(raw)
		if err != nil {
			o.fail(fmt.Errorf("text: bad unknown fixed64: %w", err))
			return nil
		}
		o.str(": 0x")
		s := strconv.FormatUint(v, 16)
		o.str(strings.Repeat("0", 16-len(s)) + s)
		o.str("\n")
		return raw[used:]
	case WireFixed32:
		v, used, err := ConsumeFixed32(raw)
		if err != nil {
			o.fail(fmt.Errorf("text: bad unknown fixed32: %w", err))
			return nil
		}
		o.str(": 0x")
		s := strconv.FormatUint(uint64(v), 16)
		o.str(strings.Repeat("0", 8-len(s)) + s)
		o.str("\n")
		return raw[used:]
	case WireLen:
		payload, used, err := ConsumeLen(raw)
		if err != nil {
			o.fail(fmt.Errorf("text: bad unknown length: %w", err))
			return nil
		}
		if textUnknownIsMessage(payload, depth) {
			o.str(" {\n")
			o.indent++
			o.printUnknowns(payload, depth+1)
			o.indent--
			o.pad()
			o.str("}\n")
		} else {
			o.str(": ")
			quoteTextString(o, payload)
			o.str("\n")
		}
		return raw[used:]
	case WireStartGroup:
		body, rest, ok := textGroupSpan(num, raw)
		if !ok {
			o.fail(fmt.Errorf("text: unterminated unknown group %d", num))
			return nil
		}
		o.str(" {\n")
		o.indent++
		o.printUnknowns(body, depth+1)
		o.indent--
		o.pad()
		o.str("}\n")
		return rest
	default:
		o.fail(fmt.Errorf("text: bad unknown wire type %d", int(wt)))
		return nil
	}
}

// textUnknownIsMessage reports whether bytes parse cleanly as a field run,
// so the unknown printer can show a length-delimited value as a message.
func textUnknownIsMessage(raw []byte, depth int) bool {
	if len(raw) == 0 {
		return false
	}
	for len(raw) > 0 {
		num, wt, used, err := ConsumeTag(raw)
		if err != nil || num == 0 || wt > WireFixed32 {
			return false
		}
		raw = raw[used:]
		switch wt {
		case WireVarint:
			if _, used, err := ConsumeVarint(raw); err != nil {
				return false
			} else {
				raw = raw[used:]
			}
		case WireFixed64:
			if len(raw) < 8 {
				return false
			}
			raw = raw[8:]
		case WireFixed32:
			if len(raw) < 4 {
				return false
			}
			raw = raw[4:]
		case WireLen:
			_, used, err := ConsumeLen(raw)
			if err != nil {
				return false
			}
			raw = raw[used:]
		case WireStartGroup:
			body, rest, ok := textGroupSpan(num, raw)
			if !ok {
				return false
			}
			_ = body
			raw = rest
		default: // end group at top level
			return false
		}
	}
	return true
}

// textGroupSpan splits an unknown group body from the bytes past its
// matching end-group tag.
func textGroupSpan(num uint32, raw []byte) (body, rest []byte, ok bool) {
	nesting := 1
	p := raw
	for len(p) > 0 {
		tagNum, wt, used, err := ConsumeTag(p)
		if err != nil || tagNum == 0 || wt > WireFixed32 {
			return nil, nil, false
		}
		p = p[used:]
		switch wt {
		case WireStartGroup:
			nesting++
		case WireEndGroup:
			nesting--
			if nesting == 0 {
				if tagNum != num {
					return nil, nil, false
				}
				return raw[:len(raw)-len(p)-used], p, true
			}
		case WireVarint:
			if _, used, err := ConsumeVarint(p); err != nil {
				return nil, nil, false
			} else {
				p = p[used:]
			}
		case WireFixed64:
			if len(p) < 8 {
				return nil, nil, false
			}
			p = p[8:]
		case WireFixed32:
			if len(p) < 4 {
				return nil, nil, false
			}
			p = p[4:]
		case WireLen:
			_, used, err := ConsumeLen(p)
			if err != nil {
				return nil, nil, false
			}
			p = p[used:]
		}
	}
	return nil, nil, false
}

/* ------------------------------------------------------------ parser */

type textIn struct {
	s     string
	pos   int
	lim   Limits
	depth int
}

// enter counts one nesting level: a message body, or a list, which costs a
// level like the reference. Past the effective cap it reports ErrDepth.
func (p *textIn) enter() error {
	if p.depth >= p.lim.effectiveDepth() {
		return ErrDepth
	}
	p.depth++
	return nil
}

func (p *textIn) fail(format string, args ...any) error {
	return fmt.Errorf("text: "+format, args...)
}

func (p *textIn) skip() {
	for p.pos < len(p.s) {
		switch p.s[p.pos] {
		case ' ', '\t', '\n', '\v', '\f', '\r':
			p.pos++
		case '#':
			for p.pos < len(p.s) && p.s[p.pos] != '\n' {
				p.pos++
			}
		default:
			return
		}
	}
}

func isTextIdentStart(c byte) bool {
	return c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c == '_'
}

func isTextIdentChar(c byte) bool {
	return isTextIdentStart(c) || c >= '0' && c <= '9'
}

func isTextDigit(c byte) bool { return c >= '0' && c <= '9' }

func isTextHex(c byte) bool {
	return isTextDigit(c) || c >= 'a' && c <= 'f' || c >= 'A' && c <= 'F'
}

// parseIdent reads an identifier after skipping space and comments.
func (p *textIn) parseIdent() (string, bool) {
	p.skip()
	start := p.pos
	if start >= len(p.s) || !isTextIdentStart(p.s[start]) {
		return "", false
	}
	for p.pos < len(p.s) && isTextIdentChar(p.s[p.pos]) {
		p.pos++
	}
	return p.s[start:p.pos], true
}

type textNumKind int

const (
	textNumInt textNumKind = iota
	textNumFloat
)

type textNum struct {
	text  string
	kind  textNumKind
	isHex bool
	isOct bool
}

// lexNumber reads a number starting at a digit, mirroring the reference
// tokenizer: hex needs digits, octal is a leading zero, `e` needs exponent
// digits, `f/F` makes a float.
func (p *textIn) lexNumber() (textNum, bool) {
	var n textNum
	q := p.pos
	s := p.s
	if s[q] == '0' && q+1 < len(s) && (s[q+1] == 'x' || s[q+1] == 'X') {
		r := q + 2
		for r < len(s) && isTextHex(s[r]) {
			r++
		}
		if r == q+2 {
			return n, false
		}
		n = textNum{text: s[q:r], kind: textNumInt, isHex: true}
		p.pos = r
		return n, p.numberEnd()
	}
	isFloat := false
	for q < len(s) && isTextDigit(s[q]) {
		q++
	}
	if s[p.pos] == '0' && q > p.pos+1 {
		n.isOct = true
	}
	if q < len(s) && s[q] == '.' {
		isFloat = true
		q++
		for q < len(s) && isTextDigit(s[q]) {
			q++
		}
	}
	if q < len(s) && (s[q] == 'e' || s[q] == 'E') {
		r := q + 1
		if r < len(s) && (s[r] == '-' || s[r] == '+') {
			r++
		}
		if r >= len(s) || !isTextDigit(s[r]) {
			// A dangling exponent is a malformed number, never a mantissa
			// followed by a new token.
			n = textNum{text: s[p.pos:q], kind: textNumFloat, isOct: n.isOct}
			p.pos = q
			return n, p.numberEnd()
		}
		for r < len(s) && isTextDigit(s[r]) {
			r++
		}
		q = r
		isFloat = true
	}
	if q < len(s) && (s[q] == 'f' || s[q] == 'F') {
		q++
		isFloat = true
	}
	if q == p.pos {
		return n, false
	}
	if n.isOct && isFloat {
		// A leading zero makes an octal token, where a point or an
		// exponent is an error (a lone 0.5 stays a float).
		return n, false
	}
	if isFloat {
		n.kind = textNumFloat
	}
	n.text = s[p.pos:q]
	p.pos = q
	return n, p.numberEnd()
}

// numberEnd rejects a letter glued to a number ("need space").
func (p *textIn) numberEnd() bool {
	return p.pos >= len(p.s) || !isTextIdentStart(p.s[p.pos])
}

// parseNumber reads one number token after skipping space and comments.
func (p *textIn) parseNumber() (textNum, bool) {
	var n textNum
	p.skip()
	if p.pos >= len(p.s) {
		return n, false
	}
	if isTextDigit(p.s[p.pos]) {
		return p.lexNumber()
	}
	if p.s[p.pos] == '.' && p.pos+1 < len(p.s) && isTextDigit(p.s[p.pos+1]) {
		q := p.pos + 1
		for q < len(p.s) && isTextDigit(p.s[q]) {
			q++
		}
		if q < len(p.s) && (p.s[q] == 'e' || p.s[q] == 'E') {
			r := q + 1
			if r < len(p.s) && (p.s[r] == '-' || p.s[r] == '+') {
				r++
			}
			if r >= len(p.s) || !isTextDigit(p.s[r]) {
				return n, false
			}
			for r < len(p.s) && isTextDigit(p.s[r]) {
				r++
			}
			q = r
		}
		if q < len(p.s) && (p.s[q] == 'f' || p.s[q] == 'F') {
			q++
		}
		n = textNum{text: p.s[p.pos:q], kind: textNumFloat}
		p.pos = q
		return n, p.numberEnd()
	}
	return n, false
}

// parseQuoted reads one quoted string; the span includes the quotes.
func (p *textIn) parseQuoted() (string, bool) {
	p.skip()
	if p.pos >= len(p.s) || (p.s[p.pos] != '"' && p.s[p.pos] != '\'') {
		return "", false
	}
	quote := p.s[p.pos]
	q := p.pos + 1
	for {
		if q >= len(p.s) || p.s[q] == '\n' || p.s[q] == 0 {
			return "", false
		}
		if p.s[q] == '\\' {
			q++
			if q >= len(p.s) || p.s[q] == '\n' || p.s[q] == '\r' {
				return "", false
			}
			q++
			continue
		}
		if p.s[q] == quote {
			q++
			break
		}
		q++
	}
	span := p.s[p.pos:q]
	p.pos = q
	return span, true
}

func textHexVal(c byte) uint32 {
	if c >= '0' && c <= '9' {
		return uint32(c - '0')
	}
	if c >= 'a' && c <= 'f' {
		return uint32(c-'a') + 10
	}
	return uint32(c-'A') + 10
}

// unescapeText appends one quoted span's unescaped form. Surrogate halves
// and out-of-range code points fail, like the reference.
func unescapeText(out *[]byte, span string) error {
	// Span includes both quotes.
	s := span[1 : len(span)-1]
	i := 0
	for i < len(s) {
		c := s[i]
		i++
		if c != '\\' {
			*out = append(*out, c)
			continue
		}
		if i >= len(s) {
			return fmt.Errorf("text: truncated escape")
		}
		c = s[i]
		i++
		if c >= '0' && c <= '7' {
			v := uint32(c - '0')
			for k := 0; k < 2 && i < len(s) && s[i] >= '0' && s[i] <= '7'; k++ {
				v = v*8 + uint32(s[i]-'0')
				i++
			}
			// The reference tokenizer truncates an octal escape above
			// \377 into a char; match the tokenizer.
			*out = append(*out, byte(v))
		} else if c == 'x' || c == 'X' {
			v, digits := uint32(0), 0
			for digits < 2 && i < len(s) && isTextHex(s[i]) {
				v = v*16 + textHexVal(s[i])
				i++
				digits++
			}
			if digits == 0 {
				return fmt.Errorf("text: bad \\x escape")
			}
			*out = append(*out, byte(v))
		} else if c == 'u' || c == 'U' {
			want := 4
			if c == 'U' {
				want = 8
			}
			var v uint32
			for k := 0; k < want; k++ {
				if i >= len(s) || !isTextHex(s[i]) {
					return fmt.Errorf("text: bad \\%c escape", c)
				}
				v = v*16 + textHexVal(s[i])
				i++
			}
			if v >= 0xd800 && v <= 0xdbff && i+5 < len(s) && s[i] == '\\' && s[i+1] == 'u' {
				var trail uint32
				ok := true
				for k := 0; k < 4; k++ {
					if !isTextHex(s[i+2+k]) {
						ok = false
						break
					}
					trail = trail*16 + textHexVal(s[i+2+k])
				}
				if ok && trail >= 0xdc00 && trail <= 0xdfff {
					v = 0x10000 + ((v - 0xd800) << 10) + (trail - 0xdc00)
					i += 6
				}
			}
			if v > 0x10ffff || v >= 0xd800 && v <= 0xdfff {
				return fmt.Errorf("text: bad unicode escape")
			}
			var tmp [4]byte
			n := 0
			switch {
			case v < 0x80:
				tmp[n] = byte(v)
				n = 1
			case v < 0x800:
				tmp[0], tmp[1] = byte(0xc0|(v>>6)), byte(0x80|(v&63))
				n = 2
			case v < 0x10000:
				tmp[0], tmp[1], tmp[2] = byte(0xe0|(v>>12)), byte(0x80|((v>>6)&63)), byte(0x80|(v&63))
				n = 3
			default:
				tmp[0], tmp[1], tmp[2], tmp[3] = byte(0xf0|(v>>18)), byte(0x80|((v>>12)&63)), byte(0x80|((v>>6)&63)), byte(0x80|(v&63))
				n = 4
			}
			*out = append(*out, tmp[:n]...)
		} else {
			var e byte
			switch c {
			case 'a':
				e = '\a'
			case 'b':
				e = '\b'
			case 'f':
				e = '\f'
			case 'n':
				e = '\n'
			case 'r':
				e = '\r'
			case 't':
				e = '\t'
			case 'v':
				e = '\v'
			case '\\':
				e = '\\'
			case '\'':
				e = '\''
			case '"':
				e = '"'
			case '?':
				e = '?'
			default:
				return fmt.Errorf("text: bad escape \\%c", c)
			}
			*out = append(*out, e)
		}
	}
	return nil
}

// parseTextUint parses one unsigned magnitude with the reference tokenizer
// rules: hex with 0x, octal with a leading 0 (8/9 are an error), decimal
// otherwise.
func parseTextUint(n textNum, max uint64) (uint64, error) {
	if n.kind != textNumInt {
		return 0, fmt.Errorf("text: bad number %q", n.text)
	}
	if n.isHex {
		var v uint64
		for i := 2; i < len(n.text); i++ {
			d := uint64(textHexVal(n.text[i]))
			if v > max/16 || v == max/16 && d > max%16 {
				return 0, fmt.Errorf("text: number %q out of range", n.text)
			}
			v = v*16 + d
		}
		return v, nil
	}
	base := 10
	digits := n.text
	if n.isOct {
		base = 8
		digits = digits[1:]
	}
	var v uint64
	for i := 0; i < len(digits); i++ {
		if digits[i] < '0' || digits[i] > '7' && base == 8 || digits[i] > '9' {
			return 0, fmt.Errorf("text: bad number %q", n.text)
		}
		d := uint64(digits[i] - '0')
		if d >= uint64(base) || v > max/uint64(base) || v == max/uint64(base) && d > max%uint64(base) {
			return 0, fmt.Errorf("text: number %q out of range", n.text)
		}
		v = v*uint64(base) + d
	}
	return v, nil
}

func parseTextSint(n textNum, neg bool, min int64, max uint64) (int64, error) {
	limit := max
	if neg {
		limit = max + 1
	}
	mag, err := parseTextUint(n, limit)
	if err != nil {
		return 0, err
	}
	if neg {
		if mag == max+1 {
			return min, nil
		}
		return -int64(mag), nil
	}
	return int64(mag), nil
}

// lookupTextField resolves a field by proto name, then (groups only) by the
// unqualified type name the printer emits. Extensions would ride brackets;
// the Go plans carry none, so brackets never resolve.
func lookupTextField(plan *MessagePlan, name string) (*FieldPlan, int, bool) {
	for i, fp := range plan.Fields {
		if fp.Name == name {
			return fp, i, false
		}
	}
	for i, fp := range plan.Fields {
		if fp.Group && shortName(fp.Message) == name {
			return fp, i, false
		}
	}
	if plan.Reserved[name] {
		return nil, -1, true
	}
	return nil, -1, false
}

// textOneofSet reports whether another member of the same oneof is set.
func textOneofSet(m *DMsg, ord int, fp *FieldPlan) bool {
	return oneofSet(m, ord, fp)
}

// parseBody parses a top-level message: fields until EOF.
func (p *textIn) parseBody(m *DMsg) error {
	return p.parseMessageBody(m, 0)
}

// textBracketName peeks a bracket span for error messages.
func textBracketName(p *textIn) string {
	q := p.pos + 1
	for q < len(p.s) && p.s[q] != ']' {
		q++
	}
	if q < len(p.s) {
		q++
	}
	return p.s[p.pos:q]
}

// skipReservedValue skips one value of a reserved field, guessing the shape
// like the reference: a brace block, a list, or one scalar.
func (p *textIn) skipReservedValue() error {
	p.skip()
	if p.pos < len(p.s) && (p.s[p.pos] == '{' || p.s[p.pos] == '<') {
		return p.skipValue()
	}
	if p.pos >= len(p.s) || p.s[p.pos] != ':' {
		return p.fail("expected ':' at offset %d", p.pos)
	}
	p.pos++
	return p.skipValue()
}

// skipValue skips one value whose type can only be guessed.
func (p *textIn) skipValue() error {
	if err := p.enter(); err != nil {
		return err
	}
	defer func() { p.depth-- }()
	p.skip()
	if p.pos >= len(p.s) {
		return p.fail("expected a value at offset %d", p.pos)
	}
	if p.s[p.pos] == '{' || p.s[p.pos] == '<' {
		closer := byte('}')
		if p.s[p.pos] == '<' {
			closer = byte('>')
		}
		p.pos++
		for {
			p.skip()
			if p.pos >= len(p.s) {
				return p.fail("truncated message value")
			}
			if p.s[p.pos] == closer {
				p.pos++
				return nil
			}
			if p.s[p.pos] == '[' {
				depth := 0
				for p.pos < len(p.s) {
					if p.s[p.pos] == '[' {
						depth++
					} else if p.s[p.pos] == ']' {
						depth--
						if depth == 0 {
							p.pos++
							break
						}
					}
					p.pos++
				}
				if depth != 0 {
					return p.fail("truncated extension name")
				}
			} else if _, ok := p.parseIdent(); !ok {
				return p.fail("expected a field name at offset %d", p.pos)
			}
			p.skip()
			if p.pos < len(p.s) && (p.s[p.pos] == '{' || p.s[p.pos] == '<') {
				if err := p.skipValue(); err != nil {
					return err
				}
			} else {
				if p.pos >= len(p.s) || p.s[p.pos] != ':' {
					return p.fail("expected ':' at offset %d", p.pos)
				}
				p.pos++
				if err := p.skipValue(); err != nil {
					return err
				}
			}
			p.skip()
			if p.pos < len(p.s) && (p.s[p.pos] == ',' || p.s[p.pos] == ';') {
				p.pos++
			}
		}
	}
	if p.s[p.pos] == '[' {
		p.pos++
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
				continue
			}
			if p.pos < len(p.s) && p.s[p.pos] == ']' {
				p.pos++
				return nil
			}
			return p.fail("expected ',' or ']' at offset %d", p.pos)
		}
	}
	if span, ok := p.parseQuoted(); ok {
		for {
			save := p.pos
			p.skip()
			if p.pos >= len(p.s) || (p.s[p.pos] != '"' && p.s[p.pos] != '\'') {
				p.pos = save
				break
			}
			if _, ok := p.parseQuoted(); !ok {
				return p.fail("bad string value")
			}
			_ = span
		}
		return nil
	}
	if p.s[p.pos] == '-' {
		p.pos++
		p.skip()
	}
	save := p.pos
	if _, ok := p.parseNumber(); ok {
		return nil
	}
	p.pos = save
	if _, ok := p.parseIdent(); ok {
		return nil
	}
	return p.fail("expected a value at offset %d", p.pos)
}

// parseMessageValue parses `{...}` or `<...>` (colon optional), appending
// for repeated fields and storing fresh for singular ones.
func (p *textIn) parseMessageValue(m *DMsg, ord int, fp *FieldPlan) error {
	if err := p.enter(); err != nil {
		return err
	}
	defer func() { p.depth-- }()
	p.skip()
	if p.pos < len(p.s) && p.s[p.pos] == ':' {
		p.pos++
		p.skip()
	}
	// Like the reference, a repeated message or group also accepts the
	// list form (`subs [{...}, ...]`); a singular one never does.
	if fp.Repeated {
		save := p.pos
		p.skip()
		if p.pos < len(p.s) && p.s[p.pos] == '[' {
			p.pos = save
			return p.parseList(m, ord, fp)
		}
		p.pos = save
	}
	if p.pos >= len(p.s) || (p.s[p.pos] != '{' && p.s[p.pos] != '<') {
		return p.fail("expected '{' for field %q", fp.Name)
	}
	closer := byte('}')
	if p.s[p.pos] == '<' {
		closer = byte('>')
	}
	p.pos++
	sub, err := m.typ.sub(ord)
	if err != nil || sub == nil {
		return p.fail("bad message field %q", fp.Name)
	}
	inner := sub.New()
	if err := p.parseMessageBody(inner, closer); err != nil {
		return err
	}
	storeMessage(m, ord, fp, inner)
	return nil
}

// parseMessageBody parses fields until the closer (or EOF for closer 0).
func (p *textIn) parseMessageBody(m *DMsg, closer byte) error {
	for {
		p.skip()
		if p.pos >= len(p.s) {
			if closer == 0 {
				return nil
			}
			return p.fail("truncated message")
		}
		if closer != 0 && p.s[p.pos] == closer {
			p.pos++
			return nil
		}
		if p.s[p.pos] == '[' {
			return p.fail("unknown extension %q", textBracketName(p))
		}
		name, ok := p.parseIdent()
		if !ok {
			return p.fail("expected a field name at offset %d", p.pos)
		}
		fp, ord, reserved := lookupTextField(m.typ.plan, name)
		if fp == nil {
			if reserved {
				if err := p.skipReservedValue(); err != nil {
					return err
				}
				p.skip()
				if p.pos < len(p.s) && (p.s[p.pos] == ',' || p.s[p.pos] == ';') {
					p.pos++
				}
				continue
			}
			return p.fail("unknown field %q", name)
		}
		if !fp.Repeated {
			if m.slots[ord].present {
				return p.fail("duplicate field %q", name)
			}
			if fp.Presence == PresenceOneof && textOneofSet(m, ord, fp) {
				return p.fail("duplicate oneof member %q", name)
			}
		}
		var err error
		if fp.Proto == descriptorpb.FieldDescriptorProto_TYPE_MESSAGE || fp.Group {
			err = p.parseMessageValue(m, ord, fp)
		} else {
			p.skip()
			if p.pos >= len(p.s) || p.s[p.pos] != ':' {
				return p.fail("expected ':' after field %q", name)
			}
			p.pos++
			if fp.Repeated {
				save := p.pos
				p.skip()
				if p.pos < len(p.s) && p.s[p.pos] == '[' {
					p.pos = save
					err = p.parseList(m, ord, fp)
				} else {
					p.pos = save
					err = p.parseScalarValue(m, ord, fp)
				}
			} else {
				err = p.parseScalarValue(m, ord, fp)
			}
		}
		if err != nil {
			return err
		}
		p.skip()
		if p.pos < len(p.s) && (p.s[p.pos] == ',' || p.s[p.pos] == ';') {
			p.pos++
		}
	}
}

// parseList parses `[a, b]` for a repeated field.
func (p *textIn) parseList(m *DMsg, ord int, fp *FieldPlan) error {
	if err := p.enter(); err != nil {
		return err
	}
	defer func() { p.depth-- }()
	p.skip()
	if p.pos >= len(p.s) || p.s[p.pos] != '[' {
		return p.fail("expected '[' for field %q", fp.Name)
	}
	p.pos++
	p.skip()
	if p.pos < len(p.s) && p.s[p.pos] == ']' {
		p.pos++
		return nil
	}
	for {
		var err error
		if fp.Proto == descriptorpb.FieldDescriptorProto_TYPE_MESSAGE || fp.Group {
			p.skip()
			if p.pos >= len(p.s) || (p.s[p.pos] != '{' && p.s[p.pos] != '<') {
				return p.fail("expected '{' for field %q", fp.Name)
			}
			closer := byte('}')
			if p.s[p.pos] == '<' {
				closer = byte('>')
			}
			p.pos++
			sub, serr := m.typ.sub(ord)
			if serr != nil || sub == nil {
				return p.fail("bad message field %q", fp.Name)
			}
			inner := sub.New()
			if err = p.parseMessageBody(inner, closer); err != nil {
				return err
			}
			storeMessage(m, ord, fp, inner)
		} else {
			err = p.parseScalarValue(m, ord, fp)
		}
		if err != nil {
			return err
		}
		p.skip()
		if p.pos < len(p.s) && p.s[p.pos] == ',' {
			p.pos++
			p.skip()
			if p.pos < len(p.s) && p.s[p.pos] == ']' {
				return p.fail("trailing comma in list for field %q", fp.Name)
			}
			continue
		}
		if p.pos < len(p.s) && p.s[p.pos] == ']' {
			p.pos++
			return nil
		}
		return p.fail("expected ',' or ']' for field %q", fp.Name)
	}
}

// parseScalarValue parses one scalar (non-message) value of the field.
func (p *textIn) parseScalarValue(m *DMsg, ord int, fp *FieldPlan) error {
	switch fp.Proto {
	case descriptorpb.FieldDescriptorProto_TYPE_DOUBLE:
		return p.parseTextFloat(m, ord, fp, false)
	case descriptorpb.FieldDescriptorProto_TYPE_FLOAT:
		return p.parseTextFloat(m, ord, fp, true)
	case descriptorpb.FieldDescriptorProto_TYPE_BOOL:
		return p.parseTextBool(m, ord, fp)
	case descriptorpb.FieldDescriptorProto_TYPE_ENUM:
		return p.parseTextEnum(m, ord, fp)
	case descriptorpb.FieldDescriptorProto_TYPE_STRING,
		descriptorpb.FieldDescriptorProto_TYPE_BYTES:
		return p.parseTextString(m, ord, fp)
	default:
		return p.parseTextInt(m, ord, fp)
	}
}

func (p *textIn) parseTextInt(m *DMsg, ord int, fp *FieldPlan) error {
	p.skip()
	neg := false
	if p.pos < len(p.s) && p.s[p.pos] == '-' {
		neg = true
		p.pos++
	}
	n, ok := p.parseNumber()
	if !ok {
		return p.fail("expected a number for field %q", fp.Name)
	}
	var umax uint64
	var smin int64
	signed := false
	switch fp.Proto {
	case descriptorpb.FieldDescriptorProto_TYPE_INT32,
		descriptorpb.FieldDescriptorProto_TYPE_SINT32,
		descriptorpb.FieldDescriptorProto_TYPE_SFIXED32:
		smin, umax, signed = math.MinInt32, math.MaxInt32, true
	case descriptorpb.FieldDescriptorProto_TYPE_INT64,
		descriptorpb.FieldDescriptorProto_TYPE_SINT64,
		descriptorpb.FieldDescriptorProto_TYPE_SFIXED64:
		smin, umax, signed = math.MinInt64, math.MaxInt64, true
	case descriptorpb.FieldDescriptorProto_TYPE_UINT32,
		descriptorpb.FieldDescriptorProto_TYPE_FIXED32:
		umax = math.MaxUint32
	case descriptorpb.FieldDescriptorProto_TYPE_UINT64,
		descriptorpb.FieldDescriptorProto_TYPE_FIXED64:
		umax = math.MaxUint64
	case descriptorpb.FieldDescriptorProto_TYPE_BOOL:
		umax = 1
	default:
		return p.fail("expected an integer for field %q", fp.Name)
	}
	if !signed && neg {
		return p.fail("bad number for field %q", fp.Name)
	}
	if signed {
		v, err := parseTextSint(n, neg, smin, umax)
		if err != nil {
			return err
		}
		storeUint(m, ord, fp, uint64(v))
		return nil
	}
	v, err := parseTextUint(n, umax)
	if err != nil {
		return err
	}
	storeUint(m, ord, fp, v)
	return nil
}

func (p *textIn) parseTextFloat(m *DMsg, ord int, fp *FieldPlan, isFloat bool) error {
	p.skip()
	neg := false
	if p.pos < len(p.s) && p.s[p.pos] == '-' {
		neg = true
		p.pos++
		p.skip()
	}
	var v float64
	if p.pos < len(p.s) && (isTextDigit(p.s[p.pos]) ||
		(p.s[p.pos] == '.' && p.pos+1 < len(p.s) && isTextDigit(p.s[p.pos+1]))) {
		n, ok := p.parseNumber()
		if !ok {
			return p.fail("expected a number for field %q", fp.Name)
		}
		if n.kind != textNumFloat && (n.isHex || n.isOct) {
			return p.fail("bad float for field %q", fp.Name)
		}
		if n.kind != textNumFloat {
			got, err := strconv.ParseFloat(n.text, 64)
			if err != nil {
				return p.fail("bad float for field %q", fp.Name)
			}
			v = got
		} else {
			text := n.text
			if len(text) > 0 && (text[len(text)-1] == 'f' || text[len(text)-1] == 'F') {
				text = text[:len(text)-1]
			}
			got, err := strconv.ParseFloat(text, 64)
			if err != nil {
				return p.fail("bad float for field %q", fp.Name)
			}
			v = got
		}
	} else {
		name, ok := p.parseIdent()
		if !ok {
			return p.fail("expected a number for field %q", fp.Name)
		}
		switch strings.ToLower(name) {
		case "inf", "infinity":
			v = math.Inf(1)
		case "nan":
			v = math.NaN()
		default:
			return p.fail("bad float for field %q", fp.Name)
		}
	}
	if neg {
		v = -v
	}
	if isFloat {
		storeUint(m, ord, fp, uint64(math.Float32bits(float32(v))))
	} else {
		storeUint(m, ord, fp, math.Float64bits(v))
	}
	return nil
}

func (p *textIn) parseTextBool(m *DMsg, ord int, fp *FieldPlan) error {
	p.skip()
	if p.pos < len(p.s) && (isTextDigit(p.s[p.pos]) || p.s[p.pos] == '-') {
		return p.parseTextInt(m, ord, fp)
	}
	name, ok := p.parseIdent()
	if !ok {
		return p.fail("expected a bool for field %q", fp.Name)
	}
	switch name {
	case "true", "True", "t":
		storeUint(m, ord, fp, 1)
		return nil
	case "false", "False", "f":
		storeUint(m, ord, fp, 0)
		return nil
	}
	return p.fail("bad bool for field %q", fp.Name)
}

func (p *textIn) parseTextEnum(m *DMsg, ord int, fp *FieldPlan) error {
	ep, err := m.typ.s.Enum(fp.Enum)
	if err != nil {
		return p.fail("unknown enum %s", fp.Enum)
	}
	known := func(num int32) bool {
		_, ok := ep.Values[num]
		return ok
	}
	finish := func(num int32) error {
		if known(num) || !ep.Closed {
			storeUint(m, ord, fp, uint64(int64(num)))
			return nil
		}
		// Unknown numbers are kept only by open enums, like on the wire.
		return p.fail("unknown enum number %d for field %q", num, fp.Name)
	}
	p.skip()
	if p.pos < len(p.s) && (isTextDigit(p.s[p.pos]) || p.s[p.pos] == '-') {
		neg := false
		if p.s[p.pos] == '-' {
			neg = true
			p.pos++
		}
		n, ok := p.parseNumber()
		if !ok {
			return p.fail("expected an enum for field %q", fp.Name)
		}
		v, err := parseTextSint(n, neg, math.MinInt32, math.MaxInt32)
		if err != nil {
			return err
		}
		return finish(int32(v))
	}
	name, ok := p.parseIdent()
	if !ok {
		return p.fail("expected an enum for field %q", fp.Name)
	}
	byName := ep.ByName
	if byName == nil {
		byName = map[string]int32{}
		for num, ename := range ep.Values {
			byName[ename] = num
		}
	}
	if num, ok := byName[name]; ok {
		return finish(num)
	}
	return p.fail("unknown enum name %q", name)
}

func (p *textIn) parseTextString(m *DMsg, ord int, fp *FieldPlan) error {
	var out []byte
	for {
		span, ok := p.parseQuoted()
		if !ok {
			return p.fail("expected a string for field %q", fp.Name)
		}
		if err := unescapeText(&out, span); err != nil {
			return err
		}
		// Adjacent literals concatenate into one value.
		save := p.pos
		p.skip()
		if p.pos >= len(p.s) || (p.s[p.pos] != '"' && p.s[p.pos] != '\'') {
			p.pos = save
			break
		}
	}
	if fp.Proto == descriptorpb.FieldDescriptorProto_TYPE_STRING && !utf8.Valid(out) {
		return p.fail("string field %q is not valid UTF-8", fp.Name)
	}
	if fp.Proto == descriptorpb.FieldDescriptorProto_TYPE_STRING {
		storeString(m, ord, fp, string(out))
	} else {
		storeBytes(m, ord, fp, out)
	}
	return nil
}

// tCheckRequired rejects trees missing a proto2 required field, recursing
// into present submessages like the binary decoder does.
func tCheckRequired(m *DMsg) error {
	for i, fp := range m.typ.plan.Fields {
		if fp.Presence == PresenceRequired && !m.slots[i].present {
			return fmt.Errorf("text: missing required field %q", fp.Name)
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
					if err := tCheckRequired(sub); err != nil {
						return err
					}
				}
			}
			continue
		}
		if sub := m.slots[i].m; sub != nil {
			if err := tCheckRequired(sub); err != nil {
				return err
			}
		}
	}
	return nil
}
