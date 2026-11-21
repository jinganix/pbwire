// Schema plans: FileDescriptorSet bytes in, immutable per-message field
// plans out. Generated code carries no baked-in field numbers; at init it
// calls Load on its embedded descriptor set and looks each message up by
// full proto name. Plans are read-only after Load returns and safe for
// concurrent use.
//
// Only the descriptor types and proto.Unmarshal come from
// google.golang.org/protobuf; nothing here runs in the encode/decode hot
// path, so reflection is fine.
package runtime

import (
	"fmt"
	"sort"
	"strings"
	"sync"

	"google.golang.org/protobuf/proto"
	"google.golang.org/protobuf/types/descriptorpb"
)

// Presence is how a field's absence is told apart from its zero value.
type Presence int

const (
	// PresenceImplicit: proto3 singular scalar without presence; absent
	// reads as the zero value and the zero value is not serialized.
	PresenceImplicit Presence = iota
	// PresenceExplicit: proto2 optional scalar, singular message/group, or
	// any field with explicit presence outside a oneof or proto3 optional.
	// Generated code holds a pointer (or nil for messages).
	PresenceExplicit
	// PresenceRequired: proto2 required. Always serialized. The codec does
	// not enforce presence on decode; verify-style checks stay with the
	// caller, as on the dynamic path today.
	PresenceRequired
	// PresenceProto3Optional: proto3 optional scalar/message. Explicit
	// presence with the proto3_optional marker; the synthetic oneof the
	// marker creates is ignored.
	PresenceProto3Optional
	// PresenceOneof: member of a real (non-synthetic) oneof. Generated code
	// holds a wrapper struct on the oneof interface field.
	PresenceOneof
)

// String names the presence rule for debug output.
func (p Presence) String() string {
	switch p {
	case PresenceImplicit:
		return "implicit"
	case PresenceExplicit:
		return "explicit"
	case PresenceRequired:
		return "required"
	case PresenceProto3Optional:
		return "proto3_optional"
	case PresenceOneof:
		return "oneof"
	default:
		return "unknown"
	}
}

// FieldPlan is one field's codec facts. Message, Enum and MapValueMsg hold
// fully qualified proto names; Name is the proto field name and Oneof is
// the bare oneof name.
type FieldPlan struct {
	Name        string                                 // proto field name
	Number      uint32                                 // field number
	Proto       descriptorpb.FieldDescriptorProto_Type // declared proto type
	Wire        WireType                               // natural wire type (packed fields ride WireLen when packed)
	Presence    Presence
	Repeated    bool
	Packed      bool // packed encoding on the wire
	IsMap       bool
	MapKey      descriptorpb.FieldDescriptorProto_Type // valid when IsMap
	MapValue    descriptorpb.FieldDescriptorProto_Type // valid when IsMap
	MapValueMsg string                                 // full message name when a map value is message-typed
	Message     string                                 // full name for message/group fields ("" otherwise)
	Enum        string                                 // full name for enum fields ("" otherwise)
	Oneof       string                                 // containing oneof, "" when not a real oneof member
	Group       bool                                   // TYPE_GROUP: start/end group framing, not length-delimited
	UTF8        bool                                   // proto3 strings reject invalid UTF-8 on decode; proto2 strings never do
	HasDefault  bool                                   // proto2 explicit default (informational; absent still reads as zero)
	Default     string                                 // raw default text from the descriptor
	JSONName    string                                 // ToJsonName of Name, memoized at plan build for the JSON codec
}

// MessagePlan is one message's ordered field list plus lookup indexes.
type MessagePlan struct {
	Full       string          // fully qualified proto name
	Fields     []*FieldPlan    // declaration order
	Oneofs     []string        // distinct real oneof names, declaration order
	IsMapEntry bool            // this message is a map entry (key/value are Fields 1/2)
	Reserved   map[string]bool // reserved field names, skipped silently by the text parser
	byNumber   map[uint32]*FieldPlan
	byName     map[string]*FieldPlan
	// Memoized JSON indexes, built once in fillMessage. The JSON codec
	// resolves every key and prints every field through these; all three
	// maps are either present together or all nil (hand-built plans fall
	// back to the linear scans in the JSON codec).
	byCamel  map[string]int // camelCase key -> ordinal, first wins
	byJSON   map[string]int // ToJsonName key -> ordinal, first wins
	ordByNum map[uint32]int // field number -> ordinal
	numOrder []int          // ordinals sorted by field number, for JSON print order
}

// ByNumber returns the field plan for a wire field number.
func (m *MessagePlan) ByNumber(n uint32) (*FieldPlan, bool) {
	f, ok := m.byNumber[n]
	return f, ok
}

// ByName returns the field plan for a proto field name.
func (m *MessagePlan) ByName(name string) (*FieldPlan, bool) {
	f, ok := m.byName[name]
	return f, ok
}

// EnumPlan is one enum's values plus its closed-ness (proto2 enums are
// closed; the codec itself still preserves unknown values as int32, since
// generated enum types are int32-based, and verify-style rejection stays
// with the caller).
type EnumPlan struct {
	Full   string
	Closed bool             // true under proto2 syntax
	Values map[int32]string // first name wins on aliases
	ByName map[string]int32 // every name, aliases included, to its number
}

// Schema is a loaded, immutable set of message, enum and service plans.
// The dynamic type cache is the one mutable part: Schema.Type builds and
// memoizes one DType per message, so the plans themselves are still
// read-only.
type Schema struct {
	messages map[string]*MessagePlan
	enums    map[string]*EnumPlan
	services map[string]*ServicePlan
	exts     map[string]*ExtPlan
	types    sync.Map // full name -> *DType
}

// ExtPlan is one proto extension: its fully qualified field name, the
// message it extends, and the field plan describing its value shape.
// Only the text codec resolves extensions today (binary keeps them as
// unknown bytes, JSON rejects them); the plan still carries the full
// shape so text parse, print and binary encode share it.
type ExtPlan struct {
	Full     string // fully qualified field name, without the leading dot
	Extendee string // fully qualified extended message, without the leading dot
	Field    *FieldPlan
}

// Extension returns the plan for a fully qualified extension field name.
func (s *Schema) Extension(full string) (*ExtPlan, bool) {
	if s == nil {
		return nil, false
	}
	xp, ok := s.exts[full]
	return xp, ok
}

// ServicePlan is one service: its methods in declaration order plus lookup.
// The core does no RPC; a transport resolves method names to input/output
// types through these and frames the bytes itself.
type ServicePlan struct {
	Full    string // fully qualified service name, without the leading dot
	Methods []*MethodPlan
	byName  map[string]*MethodPlan
}

// Method returns the plan for a method name.
func (p *ServicePlan) Method(name string) (*MethodPlan, error) {
	if p == nil {
		return nil, fmt.Errorf("codec: nil service")
	}
	m, ok := p.byName[name]
	if !ok {
		return nil, fmt.Errorf("codec: service %s has no method %q", p.Full, name)
	}
	return m, nil
}

// MethodPlan is one method: its resolved input/output types plus the
// streaming flags from the descriptor (both false is unary).
type MethodPlan struct {
	Name            string
	Input           *DType
	Output          *DType
	ClientStreaming bool
	ServerStreaming bool
}

// Load builds a Schema from raw FileDescriptorSet bytes (the exact bytes
// the generated meta sidecar embeds). It resolves nested messages, enums,
// oneofs and map entries by name; unknown type references fail loudly.
// The returned Schema is immutable and safe for concurrent use.
func Load(set []byte) (*Schema, error) {
	var fds descriptorpb.FileDescriptorSet
	if err := proto.Unmarshal(set, &fds); err != nil {
		return nil, fmt.Errorf("codec: bad descriptor set: %w", err)
	}
	s := &Schema{
		messages: map[string]*MessagePlan{},
		enums:    map[string]*EnumPlan{},
		services: map[string]*ServicePlan{},
		exts:     map[string]*ExtPlan{},
	}
	for _, f := range fds.File {
		pkg := f.GetPackage()
		for _, e := range f.EnumType {
			s.addEnum(joinName(pkg, nil, e.GetName()), e, f)
		}
		for _, m := range f.MessageType {
			s.addMessage(joinName(pkg, nil, m.GetName()), m, f)
		}
	}
	for _, f := range fds.File {
		pkg := f.GetPackage()
		for _, svc := range f.Service {
			if err := s.addService(joinName(pkg, nil, svc.GetName()), svc); err != nil {
				return nil, err
			}
		}
	}
	for _, f := range fds.File {
		pkg := f.GetPackage()
		var walk func(parent string, entriesOnly bool, ms []*descriptorpb.DescriptorProto) error
		walk = func(parent string, entriesOnly bool, ms []*descriptorpb.DescriptorProto) error {
			for _, m := range ms {
				full := parent + "." + m.GetName()
				if parent == "" {
					full = joinName(pkg, nil, m.GetName())
				}
				// Map entries nest inside the message that declares the
				// map field, but the map field resolves key/value
				// through the entry's filled field list: fill every
				// entry before any message that may reference it.
				if isEntry := s.messages[full].IsMapEntry; isEntry == entriesOnly {
					if err := s.fillMessage(full, m, f); err != nil {
						return err
					}
				}
				if err := walk(full, entriesOnly, m.NestedType); err != nil {
					return err
				}
			}
			return nil
		}
		if err := walk("", true, f.MessageType); err != nil {
			return nil, err
		}
		if err := walk("", false, f.MessageType); err != nil {
			return nil, err
		}
		if err := s.addFileExtensions(pkg, nil, f.Extension, f); err != nil {
			return nil, err
		}
		var scopes func(path []string, ms []*descriptorpb.DescriptorProto) error
		scopes = func(path []string, ms []*descriptorpb.DescriptorProto) error {
			for _, m := range ms {
				scope := append(path, m.GetName())
				if err := s.addFileExtensions(pkg, scope, m.Extension, f); err != nil {
					return err
				}
				if err := scopes(scope, m.NestedType); err != nil {
					return err
				}
			}
			return nil
		}
		if err := scopes(nil, f.MessageType); err != nil {
			return nil, err
		}
	}
	return s, nil
}

// Message returns the plan for a fully qualified message name.
func (s *Schema) Message(full string) (*MessagePlan, error) {
	m, ok := s.messages[full]
	if !ok {
		return nil, fmt.Errorf("codec: unknown message %q", full)
	}
	return m, nil
}

// Enum returns the plan for a fully qualified enum name.
func (s *Schema) Enum(full string) (*EnumPlan, error) {
	e, ok := s.enums[full]
	if !ok {
		return nil, fmt.Errorf("codec: unknown enum %q", full)
	}
	return e, nil
}

// Service returns the plan for a fully qualified service name.
func (s *Schema) Service(full string) (*ServicePlan, error) {
	p, ok := s.services[full]
	if !ok {
		return nil, fmt.Errorf("codec: unknown service %q", full)
	}
	return p, nil
}

// addService registers one service, resolving every method's input and
// output to its message type. All message plans exist by now (every file's
// messages are added before any service resolves), so an unknown type name
// fails the whole Load loudly, like an unknown field type reference.
// addFileExtensions registers one scope's extend block: each entry names
// its extendee and carries a regular field descriptor, so the field plan
// builder fills it like any other field. Unknown type references fail
// loudly, like unknown message fields do.
func (s *Schema) addFileExtensions(pkg string, scope []string, exts []*descriptorpb.FieldDescriptorProto, f *descriptorpb.FileDescriptorProto) error {
	for _, fd := range exts {
		full := joinName(pkg, scope, fd.GetName())
		if _, dup := s.exts[full]; dup {
			return fmt.Errorf("codec: duplicate extension %s", full)
		}
		fp, err := s.buildField(syntaxOf(f), nil, fd)
		if err != nil {
			return fmt.Errorf("codec: extension %s: %w", full, err)
		}
		s.exts[full] = &ExtPlan{Full: full, Extendee: normName(fd.GetExtendee()), Field: fp}
	}
	return nil
}

func (s *Schema) addService(full string, svc *descriptorpb.ServiceDescriptorProto) error {
	if _, dup := s.services[full]; dup {
		return fmt.Errorf("codec: duplicate service %s", full)
	}
	sp := &ServicePlan{Full: full, byName: map[string]*MethodPlan{}}
	for _, md := range svc.Method {
		in, err := s.Type(normName(md.GetInputType()))
		if err != nil {
			return fmt.Errorf("codec: service %s method %s: %w", full, md.GetName(), err)
		}
		out, err := s.Type(normName(md.GetOutputType()))
		if err != nil {
			return fmt.Errorf("codec: service %s method %s: %w", full, md.GetName(), err)
		}
		mp := &MethodPlan{
			Name:            md.GetName(),
			Input:           in,
			Output:          out,
			ClientStreaming: md.GetClientStreaming(),
			ServerStreaming: md.GetServerStreaming(),
		}
		if _, dup := sp.byName[mp.Name]; dup {
			return fmt.Errorf("codec: service %s has duplicate method %s", full, mp.Name)
		}
		sp.byName[mp.Name] = mp
		sp.Methods = append(sp.Methods, mp)
	}
	s.services[full] = sp
	return nil
}

func joinName(pkg string, path []string, name string) string {
	all := make([]string, 0, len(path)+2)
	if pkg != "" {
		all = append(all, pkg)
	}
	all = append(all, path...)
	all = append(all, name)
	return strings.Join(all, ".")
}

func normName(t string) string {
	return strings.TrimPrefix(t, ".")
}

func syntaxOf(f *descriptorpb.FileDescriptorProto) string {
	if syn := f.GetSyntax(); syn != "" {
		return syn
	}
	// Only proto2 can spell required or explicit defaults.
	var proto2 bool
	var walk func(ms []*descriptorpb.DescriptorProto)
	walk = func(ms []*descriptorpb.DescriptorProto) {
		for _, m := range ms {
			for _, fd := range m.Field {
				if fd.GetLabel() == descriptorpb.FieldDescriptorProto_LABEL_REQUIRED ||
					fd.DefaultValue != nil {
					proto2 = true
				}
			}
			walk(m.NestedType)
		}
	}
	walk(f.MessageType)
	if proto2 {
		return "proto2"
	}
	return "proto3"
}

func (s *Schema) addEnum(full string, e *descriptorpb.EnumDescriptorProto, f *descriptorpb.FileDescriptorProto) {
	vals := map[int32]string{}
	byName := map[string]int32{}
	for _, v := range e.Value {
		n := v.GetNumber()
		if _, ok := vals[n]; !ok {
			vals[n] = v.GetName()
		}
		byName[v.GetName()] = n
	}
	s.enums[full] = &EnumPlan{Full: full, Closed: syntaxOf(f) == "proto2", Values: vals, ByName: byName}
}

func (s *Schema) addMessage(full string, m *descriptorpb.DescriptorProto, f *descriptorpb.FileDescriptorProto) {
	s.messages[full] = &MessagePlan{Full: full, IsMapEntry: m.GetOptions().GetMapEntry()}
	for _, e := range m.EnumType {
		s.addEnum(full+"."+e.GetName(), e, f)
	}
	for _, n := range m.NestedType {
		s.addMessage(full+"."+n.GetName(), n, f)
	}
}

func (s *Schema) fillMessage(full string, m *descriptorpb.DescriptorProto, f *descriptorpb.FileDescriptorProto) error {
	mp := s.messages[full]
	if mp == nil {
		return fmt.Errorf("codec: message %q vanished during plan build", full)
	}
	oneofs := make([]string, len(m.OneofDecl))
	for i, o := range m.OneofDecl {
		oneofs[i] = o.GetName()
	}
	syntax := syntaxOf(f)
	for _, fd := range m.Field {
		fp, err := s.buildField(syntax, oneofs, fd)
		if err != nil {
			return fmt.Errorf("codec: message %s field %s: %w", full, fd.GetName(), err)
		}
		mp.Fields = append(mp.Fields, fp)
	}
	mp.byNumber = make(map[uint32]*FieldPlan, len(mp.Fields))
	mp.byName = make(map[string]*FieldPlan, len(mp.Fields))
	mp.byCamel = make(map[string]int, len(mp.Fields))
	mp.byJSON = make(map[string]int, len(mp.Fields))
	mp.ordByNum = make(map[uint32]int, len(mp.Fields))
	for _, name := range m.ReservedName {
		if mp.Reserved == nil {
			mp.Reserved = map[string]bool{}
		}
		mp.Reserved[name] = true
	}
	for i, fp := range mp.Fields {
		if _, dup := mp.byNumber[fp.Number]; dup {
			return fmt.Errorf("codec: message %s has duplicate field number %d", full, fp.Number)
		}
		mp.byNumber[fp.Number] = fp
		mp.byName[fp.Name] = fp
		// First wins in each derived namespace, mirroring the linear
		// scans this replaces (they return the earliest field match).
		if _, dup := mp.byCamel[camelName(fp.Name)]; !dup {
			mp.byCamel[camelName(fp.Name)] = i
		}
		if _, dup := mp.byJSON[fp.JSONName]; !dup {
			mp.byJSON[fp.JSONName] = i
		}
		mp.ordByNum[fp.Number] = i
	}
	seen := map[string]bool{}
	for _, fp := range mp.Fields {
		if fp.Oneof != "" && !seen[fp.Oneof] {
			seen[fp.Oneof] = true
			mp.Oneofs = append(mp.Oneofs, fp.Oneof)
		}
	}
	// Field order for JSON print: declaration order is usually already
	// numeric, but out-of-order declarations are legal, so the sorted
	// index is memoized once instead of sorting per message.
	mp.numOrder = make([]int, len(mp.Fields))
	for i := range mp.Fields {
		mp.numOrder[i] = i
	}
	sort.Slice(mp.numOrder, func(a, b int) bool {
		return mp.Fields[mp.numOrder[a]].Number < mp.Fields[mp.numOrder[b]].Number
	})
	return nil
}

func wireFor(t descriptorpb.FieldDescriptorProto_Type) (WireType, bool) {
	switch t {
	case descriptorpb.FieldDescriptorProto_TYPE_INT32,
		descriptorpb.FieldDescriptorProto_TYPE_INT64,
		descriptorpb.FieldDescriptorProto_TYPE_UINT32,
		descriptorpb.FieldDescriptorProto_TYPE_UINT64,
		descriptorpb.FieldDescriptorProto_TYPE_SINT32,
		descriptorpb.FieldDescriptorProto_TYPE_SINT64,
		descriptorpb.FieldDescriptorProto_TYPE_BOOL,
		descriptorpb.FieldDescriptorProto_TYPE_ENUM:
		return WireVarint, true
	case descriptorpb.FieldDescriptorProto_TYPE_FIXED64,
		descriptorpb.FieldDescriptorProto_TYPE_SFIXED64,
		descriptorpb.FieldDescriptorProto_TYPE_DOUBLE:
		return WireFixed64, true
	case descriptorpb.FieldDescriptorProto_TYPE_STRING,
		descriptorpb.FieldDescriptorProto_TYPE_BYTES,
		descriptorpb.FieldDescriptorProto_TYPE_MESSAGE:
		return WireLen, true
	case descriptorpb.FieldDescriptorProto_TYPE_GROUP:
		return WireStartGroup, true
	case descriptorpb.FieldDescriptorProto_TYPE_FIXED32,
		descriptorpb.FieldDescriptorProto_TYPE_SFIXED32,
		descriptorpb.FieldDescriptorProto_TYPE_FLOAT:
		return WireFixed32, true
	default:
		return 0, false
	}
}

func packable(t descriptorpb.FieldDescriptorProto_Type) bool {
	switch t {
	case descriptorpb.FieldDescriptorProto_TYPE_STRING,
		descriptorpb.FieldDescriptorProto_TYPE_BYTES,
		descriptorpb.FieldDescriptorProto_TYPE_MESSAGE,
		descriptorpb.FieldDescriptorProto_TYPE_GROUP:
		return false
	default:
		return true
	}
}

func (s *Schema) buildField(syntax string, oneofs []string, fd *descriptorpb.FieldDescriptorProto) (*FieldPlan, error) {
	wt, ok := wireFor(fd.GetType())
	if !ok {
		return nil, fmt.Errorf("unknown proto type %d", int32(fd.GetType()))
	}
	fp := &FieldPlan{
		Name:     fd.GetName(),
		JSONName: jsonName(fd.GetName()),
		Number:   uint32(fd.GetNumber()),
		Proto:    fd.GetType(),
		Wire:     wt,
		Group:    fd.GetType() == descriptorpb.FieldDescriptorProto_TYPE_GROUP,
	}
	if fp.Number == 0 || fp.Number > MaxFieldNumber {
		return nil, fmt.Errorf("bad field number %d", fd.GetNumber())
	}
	if fd.DefaultValue != nil {
		fp.HasDefault = true
		fp.Default = fd.GetDefaultValue()
	}
	label := fd.GetLabel()
	fp.Repeated = label == descriptorpb.FieldDescriptorProto_LABEL_REPEATED

	// Map fields ride as repeated entry messages: resolve the entry type
	// and lift its key/value types onto the field plan. Maps never carry
	// presence and are never packed.
	if fd.GetType() == descriptorpb.FieldDescriptorProto_TYPE_MESSAGE && !fp.Group {
		if em := s.messages[normName(fd.GetTypeName())]; em != nil && em.IsMapEntry {
			keyp, ok := em.byNumber[1]
			if !ok {
				return nil, fmt.Errorf("map entry %s has no key field", em.Full)
			}
			valp, ok := em.byNumber[2]
			if !ok {
				return nil, fmt.Errorf("map entry %s has no value field", em.Full)
			}
			fp.IsMap = true
			fp.Repeated = true
			fp.Presence = PresenceImplicit
			fp.Message = em.Full
			fp.MapKey = keyp.Proto
			fp.MapValue = valp.Proto
			switch valp.Proto {
			case descriptorpb.FieldDescriptorProto_TYPE_MESSAGE:
				fp.MapValueMsg = valp.Message
			case descriptorpb.FieldDescriptorProto_TYPE_ENUM:
				fp.Enum = valp.Enum
			}
			return fp, nil
		}
	}

	// Presence and references next. Map entry contents are always filled
	// first (see Load), so the entry field lists below are ready.
	switch {
	case label == descriptorpb.FieldDescriptorProto_LABEL_REQUIRED:
		fp.Presence = PresenceRequired
	case fd.GetProto3Optional():
		// Proto3 optional owns a synthetic oneof; the explicit presence is
		// what matters, so the oneof index is ignored.
		fp.Presence = PresenceProto3Optional
	case fd.OneofIndex != nil && int(*fd.OneofIndex) < len(oneofs):
		fp.Presence = PresenceOneof
		fp.Oneof = oneofs[*fd.OneofIndex]
	case fp.Repeated:
		fp.Presence = PresenceImplicit
	case fd.GetType() == descriptorpb.FieldDescriptorProto_TYPE_MESSAGE || fp.Group:
		fp.Presence = PresenceExplicit
	case syntax == "proto2":
		// Proto2 optional scalars always carry explicit presence.
		fp.Presence = PresenceExplicit
	default:
		fp.Presence = PresenceImplicit
	}

	if fp.Proto == descriptorpb.FieldDescriptorProto_TYPE_MESSAGE || fp.Group {
		ref := normName(fd.GetTypeName())
		if _, ok := s.messages[ref]; !ok {
			return nil, fmt.Errorf("unknown message type %q", fd.GetTypeName())
		}
		fp.Message = ref
	}
	if fp.Proto == descriptorpb.FieldDescriptorProto_TYPE_ENUM {
		ref := normName(fd.GetTypeName())
		if _, ok := s.enums[ref]; !ok {
			return nil, fmt.Errorf("unknown enum type %q", fd.GetTypeName())
		}
		fp.Enum = ref
	}

	// Packed encoding: proto3 packs repeated packable numerics unless
	// packed=false is written explicitly (the option bit distinguishes
	// explicit false from unset); proto2 packs only on packed=true.
	if fp.Repeated && packable(fp.Proto) {
		if syntax == "proto3" {
			fp.Packed = true
			if opts := fd.GetOptions(); opts != nil && opts.Packed != nil {
				fp.Packed = opts.GetPacked()
			}
		} else {
			fp.Packed = fd.GetOptions().GetPacked()
		}
	}
	// String validation follows the dialect default (VERIFY for proto3 and
	// editions, NONE for proto2), like the reference FeatureSet default: only
	// string fields carry it, bytes never do.
	fp.UTF8 = syntax == "proto3" && fp.Proto == descriptorpb.FieldDescriptorProto_TYPE_STRING
	return fp, nil
}
