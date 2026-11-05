package gen

// Objective-C emitter: GPBMessage-shaped facades over the dynamic API.
//
// The surface mirrors what protoc --objc_out would generate for the same
// .proto (one class per message, FieldNumber enums, has-bit accessors,
// XArray + XArray_Count for repeated fields, KindOneOfCase for oneofs,
// +message / -data / +parseFromData:error: / -initWithData:error: /
// isInitialized), but every codec call goes through the binding's existing
// dynamic API (PBWMessage/PBWSchema), so the wire format stays in the C core
// and no protobuf header is needed. The FileDescriptorSet is embedded
// (base64, loaded once via dispatch_once), so there is no .desc file at use
// time. The legacy schema-backed entry points stay: examples and the README
// use them.
//
// Two deliberate deviations from the generated ObjC runtime, both forced by
// the no-protobuf-dependency rule:
//   - scalar repeated fields are NSMutableArray<NSNumber *> instead of
//     GPBInt32Array/GPBBoolArray/..., keeping the official XArray/_Count
//     names; containers are created lazily like the official ones.
//   - classes carry the PBWGen_ prefix, so one binary can link the oracle
//     (unprefixed Scalars3) next to the facade for the compat test.

import (
	"fmt"
	"strings"

	"pbwire-typed/internal/model"

	"google.golang.org/protobuf/reflect/protoreflect"
)

func objcScalar(field model.Field) string {
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind,
		protoreflect.EnumKind:
		return "int32_t"
	case protoreflect.Uint32Kind, protoreflect.Fixed32Kind:
		return "uint32_t"
	case protoreflect.Int64Kind, protoreflect.Sint64Kind, protoreflect.Sfixed64Kind:
		return "int64_t"
	case protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		return "uint64_t"
	case protoreflect.FloatKind:
		return "float"
	case protoreflect.DoubleKind:
		return "double"
	case protoreflect.BoolKind:
		return "BOOL"
	case protoreflect.StringKind:
		return "NSString *"
	case protoreflect.BytesKind:
		return "NSData *"
	default:
		return objcClass(field.Message) + " *"
	}
}

func objcClass(local string) string { return "PBWGen_" + local }

// objcElem is the NSMutableArray element class of a repeated field.
func objcElem(field model.Field) string {
	switch field.Kind {
	case protoreflect.StringKind:
		return "NSString *"
	case protoreflect.BytesKind:
		return "NSData *"
	case protoreflect.MessageKind, protoreflect.GroupKind:
		return objcClass(field.Message) + " *"
	default:
		return "NSNumber *"
	}
}

// objcArray is the official repeated-field property name: nums -> numsArray.
func objcArray(field model.Field) string { return field.Lower + "Array" }

// objcOneofs returns the distinct oneof names of a message in field order,
// skipping groups whose members are all skipped.
func objcOneofs(msg model.Message) []string {
	var out []string
	seen := map[string]bool{}
	for _, field := range msg.Fields {
		if field.Skip != "" || field.Oneof == "" || seen[field.Oneof] {
			continue
		}
		seen[field.Oneof] = true
		out = append(out, field.Oneof)
	}
	var kept []string
	for _, o := range out {
		for _, field := range msg.Fields {
			if field.Skip == "" && field.Oneof == o {
				kept = append(kept, o)
				break
			}
		}
	}
	return kept
}

func objcOneofEnum(msg model.Message, oneof string) string {
	return "PBWGen_" + msg.Name + "_" + model.Pascal(oneof) + "_OneOfCase"
}

func objcOneofUnset(msg model.Message, oneof string) string {
	return objcOneofEnum(msg, oneof) + "_GPBUnsetOneOfCase"
}

func objcOneofValue(msg model.Message, field model.Field) string {
	return objcOneofEnum(msg, field.Oneof) + "_" + field.Pascal
}

func objcOneofCaseProp(oneof string) string {
	return model.LowerCamel(oneof) + "OneOfCase"
}

// objcFiles emits one header and one implementation per proto file: typed
// GPBMessage-shaped classes over PBWSchema/PBWMessage with the file's
// FileDescriptorSet embedded.
func objcFiles(f *model.File, opt Options) []File {
	var h, m strings.Builder
	guard := "PBWGEN_" + strings.ToUpper(strings.ReplaceAll(base(f), "-", "_")) + "_H"
	h.WriteString(header(f, "GPBMessage-shaped typed classes over PBWSchema/PBWMessage (embedded FileDescriptorSet)."))
	fmt.Fprintf(&h, "#ifndef %s\n#define %s\n\n#import <Foundation/Foundation.h>\n\n#import \"PBWMessage.h\"\n#import \"PBWSchema.h\"\n\nNS_ASSUME_NONNULL_BEGIN\n\n", guard, guard)
	m.WriteString(header(f, "GPBMessage-shaped typed classes over PBWSchema/PBWMessage (embedded FileDescriptorSet)."))
	fmt.Fprintf(&m, "#import %q\n#import \"PBWError.h\"\n\nNS_ASSUME_NONNULL_BEGIN\n\n", base(f)+".h")

	// The embedded schema: this file plus its transitive imports, exactly
	// what protoc --descriptor_set_out would write for it.
	fmt.Fprintf(&m, "static NSString * const kPBWGenDescB64 =\n")
	chunks := chunk64(opt.DescSetB64)
	if len(chunks) == 0 {
		chunks = []string{""}
	}
	for _, c := range chunks {
		fmt.Fprintf(&m, "    @%q\n", c)
	}
	m.WriteString("    ;\n\nstatic PBWSchema *PBWGenFileSchema(void)\n{\n    static PBWSchema *schema = nil;\n    static dispatch_once_t onceToken;\n    dispatch_once(&onceToken, ^{\n        NSData *raw = [[NSData alloc] initWithBase64EncodedString:kPBWGenDescB64 options:0];\n        NSError *error = nil;\n        schema = [PBWSchema schemaFromData:raw error:&error];\n    });\n    return schema;\n}\n\n")

	for _, msg := range f.Messages {
		fmt.Fprintf(&h, "@class %s;\n", objcClass(msg.Name))
	}
	h.WriteString("\n")
	for _, e := range f.Enums {
		fmt.Fprintf(&h, "/** %s */\ntypedef NS_ENUM(int32_t, %s) {\n", e.Full, objcEnum(e.Name))
		for _, v := range e.Values {
			fmt.Fprintf(&h, "    %s_%s = %d,\n", objcEnum(e.Name), v.Name, v.Number)
		}
		h.WriteString("};\n\n")
	}
	for _, msg := range f.Messages {
		writeObjcMessage(&h, &m, f, msg)
	}
	h.WriteString("NS_ASSUME_NONNULL_END\n\n#endif /* " + guard + " */\n")
	m.WriteString("NS_ASSUME_NONNULL_END\n")
	return []File{
		{Name: base(f) + ".h", Content: h.String()},
		{Name: base(f) + ".m", Content: m.String()},
	}
}

// chunk64 splits s into 60-character quoted chunks (plain literals cannot
// span lines, like protoc's own C# output).
func chunk64(s string) []string {
	var out []string
	for len(s) > 0 {
		n := 60
		if n > len(s) {
			n = len(s)
		}
		out = append(out, s[:n])
		s = s[n:]
	}
	return out
}

func objcEnum(local string) string { return "PBWGen_" + local }

func writeObjcMessage(h, m *strings.Builder, f *model.File, msg model.Message) {
	cls := objcClass(msg.Name)
	kept := false
	for _, field := range msg.Fields {
		if field.Skip == "" {
			kept = true
			break
		}
	}
	if kept {
		fmt.Fprintf(h, "typedef NS_ENUM(int32_t, %s_FieldNumber) {\n", cls)
		for _, field := range msg.Fields {
			if field.Skip != "" {
				continue
			}
			name := field.Pascal
			if field.Repeated {
				name += "Array"
			}
			fmt.Fprintf(h, "    %s_FieldNumber_%s = %d,\n", cls, name, field.Number)
		}
		fmt.Fprintf(h, "};\n\n")
	}
	for _, o := range objcOneofs(msg) {
		fmt.Fprintf(h, "typedef NS_ENUM(int32_t, %s) {\n", objcOneofEnum(msg, o))
		fmt.Fprintf(h, "    %s = 0,\n", objcOneofUnset(msg, o))
		for _, field := range msg.Fields {
			if field.Skip != "" || field.Oneof != o {
				continue
			}
			fmt.Fprintf(h, "    %s = %d,\n", objcOneofValue(msg, field), field.Number)
		}
		fmt.Fprintf(h, "};\n\n")
	}
	fmt.Fprintf(h, "/** %s */\n@interface %s : NSObject\n", msg.Full, cls)
	for _, field := range msg.Fields {
		if field.Skip != "" {
			fmt.Fprintf(h, "// skipped %s: %s\n", field.Name, field.Skip)
			continue
		}
		if field.Oneof != "" {
			continue
		}
		writeObjcProp(h, msg, field, false)
	}
	for _, o := range objcOneofs(msg) {
		fmt.Fprintf(h, "@property (nonatomic, readonly) %s %s;\n", objcOneofEnum(msg, o), objcOneofCaseProp(o))
		for _, field := range msg.Fields {
			if field.Skip != "" || field.Oneof != o {
				continue
			}
			writeObjcProp(h, msg, field, true)
		}
		fmt.Fprintf(h, "- (void)clear%s;\n", model.Pascal(o))
	}
	fmt.Fprintf(h, "+ (instancetype)message;\n")
	fmt.Fprintf(h, "+ (nullable PBWSchema *)pbw_schema;\n")
	fmt.Fprintf(h, "- (nullable NSData *)data;\n")
	fmt.Fprintf(h, "+ (nullable instancetype)parseFromData:(nullable NSData *)data error:(NSError **)error;\n")
	fmt.Fprintf(h, "- (nullable instancetype)initWithData:(nullable NSData *)data error:(NSError **)error;\n")
	fmt.Fprintf(h, "@property (nonatomic, readonly, getter=isInitialized) BOOL initialized;\n")
	fmt.Fprintf(h, "- (void)loadFromMessage:(PBWMessage *)msg;\n")
	fmt.Fprintf(h, "- (BOOL)encodeInto:(PBWMessage *)msg error:(NSError **)error;\n")
	fmt.Fprintf(h, "- (nullable NSData *)encodeWithSchema:(PBWSchema *)schema error:(NSError **)error;\n")
	fmt.Fprintf(h, "+ (nullable instancetype)decodeWithSchema:(PBWSchema *)schema data:(NSData *)data error:(NSError **)error;\n")
	fmt.Fprintf(h, "+ (instancetype)fromMessage:(PBWMessage *)msg;\n@end\n\n")

	fmt.Fprintf(m, "@implementation %s\n\n", cls)
	for _, field := range msg.Fields {
		if field.Skip == "" && field.Repeated {
			fmt.Fprintf(m, "@synthesize %s = _%s;\n", objcArray(field), objcArray(field))
		}
	}
	if len(msg.Fields) > 0 {
		m.WriteString("\n")
	}
	fmt.Fprintf(m, "+ (nullable PBWSchema *)pbw_schema\n{\n    return PBWGenFileSchema();\n}\n\n")
	fmt.Fprintf(m, "+ (instancetype)message\n{\n    return [[self alloc] init];\n}\n\n")
	fmt.Fprintf(m, "- (nullable NSData *)data\n{\n")
	fmt.Fprintf(m, "    PBWSchema *schema = [[self class] pbw_schema];\n")
	m.WriteString("    if (schema == nil) { return nil; }\n")
	fmt.Fprintf(m, "    PBWMessage *msg = [schema messageByName:@%q];\n", msg.Full)
	m.WriteString("    if (msg == nil) { return nil; }\n    NSError *error = nil;\n    if (![self encodeInto:msg error:&error]) { return nil; }\n    return [msg encode:&error];\n}\n\n")
	fmt.Fprintf(m, "+ (nullable instancetype)parseFromData:(nullable NSData *)data error:(NSError **)error\n{\n    return [[self alloc] initWithData:data error:error];\n}\n\n")
	fmt.Fprintf(m, "- (nullable instancetype)initWithData:(nullable NSData *)data error:(NSError **)error\n{\n")
	m.WriteString("    self = [super init];\n    if (self == nil) { return nil; }\n")
	m.WriteString("    if (data == nil) { PBWSetError(error, PBW_ERR_INVALID, @\"nil data\"); return nil; }\n")
	fmt.Fprintf(m, "    PBWSchema *schema = [[self class] pbw_schema];\n")
	m.WriteString("    if (schema == nil) { PBWSetError(error, PBW_ERR_INVALID, @\"embedded descriptor\"); return nil; }\n")
	fmt.Fprintf(m, "    PBWMessage *msg = [schema messageByName:@%q];\n", msg.Full)
	fmt.Fprintf(m, "    if (msg == nil) { PBWSetError(error, PBW_ERR_NOT_FOUND, @%q); return nil; }\n", msg.Full)
	m.WriteString("    PBWMessage *decoded = [msg decode:data error:error];\n    if (decoded == nil) { return nil; }\n    [self loadFromMessage:decoded];\n")
	fmt.Fprintf(m, "    if (!self.initialized) { PBWSetError(error, PBW_ERR_INVALID, @\"required field missing: %s\"); return nil; }\n", msg.Full)
	m.WriteString("    return self;\n}\n\n")
	writeObjcInitialized(m, f, msg)
	writeObjcLoad(m, f, msg)
	fmt.Fprintf(m, "+ (instancetype)fromMessage:(PBWMessage *)msg\n{\n    %s *o = [[self alloc] init];\n    [o loadFromMessage:msg];\n    return o;\n}\n\n", cls)
	writeObjcEncode(m, msg)
	fmt.Fprintf(m, "- (nullable NSData *)encodeWithSchema:(PBWSchema *)schema error:(NSError **)error\n{\n")
	fmt.Fprintf(m, "    PBWMessage *msg = [schema messageByName:@%q];\n", msg.Full)
	m.WriteString("    if (msg == nil || ![self encodeInto:msg error:error]) { return nil; }\n    return [msg encode:error];\n}\n\n")
	fmt.Fprintf(m, "+ (nullable instancetype)decodeWithSchema:(PBWSchema *)schema data:(NSData *)data error:(NSError **)error\n{\n")
	fmt.Fprintf(m, "    PBWMessage *msg = [schema messageByName:@%q];\n", msg.Full)
	m.WriteString("    if (msg == nil) { return nil; }\n    PBWMessage *decoded = [msg decode:data error:error];\n    return decoded == nil ? nil : [self fromMessage:decoded];\n}\n\n")
	writeObjcAccessors(m, f, msg)
	m.WriteString("@end\n\n")
}

// writeObjcProp emits one property (plus its has-bit / count companion).
// oneofMember makes the has-bit readonly: the case enum owns presence.
func writeObjcProp(h *strings.Builder, msg model.Message, field model.Field, oneofMember bool) {
	if field.Repeated {
		fmt.Fprintf(h, "@property (nonatomic, strong, nullable) NSMutableArray<%s> *%s;\n", objcElem(field), objcArray(field))
		fmt.Fprintf(h, "@property (nonatomic, readonly) NSUInteger %s_Count;\n", objcArray(field))
		return
	}
	if field.Message != "" {
		fmt.Fprintf(h, "@property (nonatomic, strong, nullable) %s%s;\n", objcScalar(field), field.Lower)
	} else if field.Kind == protoreflect.StringKind {
		fmt.Fprintf(h, "@property (nonatomic, copy, nullable) %s%s;\n", objcScalar(field), field.Lower)
	} else if field.Kind == protoreflect.BytesKind {
		fmt.Fprintf(h, "@property (nonatomic, copy, nullable) NSData *%s;\n", field.Lower)
	} else {
		fmt.Fprintf(h, "@property (nonatomic, assign) %s %s;\n", objcScalar(field), field.Lower)
	}
	if field.NeedsHas || field.Message != "" {
		if oneofMember {
			fmt.Fprintf(h, "@property (nonatomic, readonly) BOOL has%s;\n", field.Pascal)
		} else {
			fmt.Fprintf(h, "@property (nonatomic, assign) BOOL has%s;\n", field.Pascal)
		}
	}
}

// writeObjcInitialized emits -isInitialized: required fields must be present
// (required scalars are always "present" once decoded: the typed model cannot
// distinguish an explicit zero from an absent one) and present submessages
// must themselves be initialized, like the official runtime.
func writeObjcInitialized(m *strings.Builder, f *model.File, msg model.Message) {
	m.WriteString("- (BOOL)isInitialized\n{\n")
	for _, field := range msg.Fields {
		if field.Skip != "" || !field.Required || field.Oneof != "" || field.Repeated {
			continue
		}
		if field.Message != "" || field.Kind == protoreflect.StringKind || field.Kind == protoreflect.BytesKind {
			fmt.Fprintf(m, "    if (self.%s == nil) { return NO; }\n", field.Lower)
		}
	}
	for _, field := range msg.Fields {
		if field.Skip != "" || field.Message == "" {
			continue
		}
		if field.Repeated {
			fmt.Fprintf(m, "    for (%s v in self.%s) { if (![v isInitialized]) { return NO; } }\n", objcClass(field.Message)+" *", objcArray(field))
		} else {
			fmt.Fprintf(m, "    if (self.%s != nil && ![self.%s isInitialized]) { return NO; }\n", field.Lower, field.Lower)
		}
	}
	m.WriteString("    return YES;\n}\n\n")
}

// writeObjcLoad emits -loadFromMessage: (decode). Oneof members assign in
// field order through their setters, so the last present member wins, like
// the last wire tag wins in the official runtime.
func writeObjcLoad(m *strings.Builder, f *model.File, msg model.Message) {
	m.WriteString("- (void)loadFromMessage:(PBWMessage *)msg\n{\n")
	for _, field := range msg.Fields {
		if field.Skip != "" {
			continue
		}
		if field.Repeated {
			fmt.Fprintf(m, "    NSUInteger n%s = [msg countForField:@%q];\n", field.Pascal, field.Name)
			fmt.Fprintf(m, "    if (n%s > 0) {\n", field.Pascal)
			fmt.Fprintf(m, "        NSMutableArray<%s> *a%s = [NSMutableArray arrayWithCapacity:n%s];\n", objcElem(field), field.Pascal, field.Pascal)
			fmt.Fprintf(m, "        for (NSUInteger i = 0; i < n%s; i++) {\n", field.Pascal)
			fmt.Fprintf(m, "            id v = [msg valueForField:@%q atIndex:i];\n", field.Name)
			if field.Message != "" {
				fmt.Fprintf(m, "            if ([v isKindOfClass:[PBWMessage class]]) { [a%s addObject:[%s fromMessage:v]]; }\n", field.Pascal, objcClass(field.Message))
			} else {
				fmt.Fprintf(m, "            if (v != nil) { [a%s addObject:v]; }\n", field.Pascal)
			}
			fmt.Fprintf(m, "        }\n        self.%s = a%s;\n    }\n", objcArray(field), field.Pascal)
			continue
		}
		writeObjcRead(m, f, field)
	}
	m.WriteString("}\n\n")
}

// writeObjcEncode emits -encodeInto:. Optional and message setters mark
// presence, so assigning a value is enough; the guards below only skip work.
func writeObjcEncode(m *strings.Builder, msg model.Message) {
	m.WriteString("- (BOOL)encodeInto:(PBWMessage *)msg error:(NSError **)error\n{\n")
	for _, field := range msg.Fields {
		if field.Skip != "" || field.Oneof != "" {
			continue
		}
		if field.Repeated {
			fmt.Fprintf(m, "    if (self.%s.count > 0) {\n", objcArray(field))
			if field.Message != "" {
				fmt.Fprintf(m, "        for (%s v in self.%s) {\n", objcClass(field.Message)+" *", objcArray(field))
				fmt.Fprintf(m, "            PBWMessage *child = [msg addMessageForField:@%q error:error];\n", field.Name)
				m.WriteString("            if (child == nil || ![v encodeInto:child error:error]) { return NO; }\n        }\n")
			} else {
				fmt.Fprintf(m, "        for (%s v in self.%s) {\n", objcElem(field), objcArray(field))
				fmt.Fprintf(m, "            if (![msg addValue:v forField:@%q error:error]) { return NO; }\n", field.Name)
				m.WriteString("        }\n")
			}
			m.WriteString("    }\n")
			continue
		}
		cond := objcPresent(field)
		fmt.Fprintf(m, "    if (%s) {\n", cond)
		if field.Message != "" {
			fmt.Fprintf(m, "        PBWMessage *child = [msg addMessageForField:@%q error:error];\n", field.Name)
			fmt.Fprintf(m, "        if (child == nil || ![self.%s encodeInto:child error:error]) { return NO; }\n", field.Lower)
		} else {
			fmt.Fprintf(m, "        if (![msg setValue:%s forField:@%q error:error]) { return NO; }\n", objcBox(field, "self."+field.Lower), field.Name)
		}
		m.WriteString("    }\n")
	}
	for _, o := range objcOneofs(msg) {
		fmt.Fprintf(m, "    switch (self.%s) {\n", objcOneofCaseProp(o))
		for _, field := range msg.Fields {
			if field.Skip != "" || field.Oneof != o {
				continue
			}
			fmt.Fprintf(m, "        case %s:\n", objcOneofValue(msg, field))
			if field.Message != "" {
				fmt.Fprintf(m, "            {\n                PBWMessage *child = [msg addMessageForField:@%q error:error];\n", field.Name)
				fmt.Fprintf(m, "                if (child == nil || ![self.%s encodeInto:child error:error]) { return NO; }\n            }\n", field.Lower)
			} else {
				fmt.Fprintf(m, "            if (![msg setValue:%s forField:@%q error:error]) { return NO; }\n", objcBox(field, "self."+field.Lower), field.Name)
			}
			m.WriteString("            break;\n")
		}
		fmt.Fprintf(m, "        case %s:\n            break;\n    }\n", objcOneofUnset(msg, o))
	}
	m.WriteString("    return YES;\n}\n\n")
}

func objcPresent(field model.Field) string {
	if field.Required {
		return "1"
	}
	switch {
	case field.Message != "":
		return "self." + field.Lower + " != nil"
	case field.NeedsHas:
		return "self.has" + field.Pascal
	case field.Kind == protoreflect.StringKind:
		return "self." + field.Lower + ".length > 0"
	case field.Kind == protoreflect.BytesKind:
		return "self." + field.Lower + ".length > 0"
	case field.Kind == protoreflect.BoolKind:
		return "self." + field.Lower
	default:
		return "self." + field.Lower + " != 0"
	}
}

func objcBox(field model.Field, expr string) string {
	switch field.Kind {
	case protoreflect.StringKind, protoreflect.BytesKind:
		return expr
	case protoreflect.Uint32Kind, protoreflect.Fixed32Kind:
		/* The typed property holds raw bits; the wrapper wants the unsigned value. */
		return "@((uint32_t)" + expr + ")"
	case protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		return "@((uint64_t)" + expr + ")"
	default:
		return "@(" + expr + ")"
	}
}

func writeObjcRead(m *strings.Builder, f *model.File, field model.Field) {
	if field.Message != "" {
		fmt.Fprintf(m, "    PBWMessage *c%s = [msg messageForField:@%q];\n", field.Pascal, field.Name)
		// The setter marks presence (has-bit or oneof case), so a bare
		// assignment is the whole load.
		fmt.Fprintf(m, "    if (c%s != nil) { self.%s = [%s fromMessage:c%s]; }\n", field.Pascal, field.Lower, objcClass(field.Message), field.Pascal)
		return
	}
	fmt.Fprintf(m, "    id v%s = [msg valueForField:@%q];\n", field.Pascal, field.Name)
	switch field.Kind {
	case protoreflect.StringKind:
		fmt.Fprintf(m, "    if ([v%s isKindOfClass:[NSString class]] || v%s == nil) { self.%s = v%s; }\n", field.Pascal, field.Pascal, field.Lower, field.Pascal)
	case protoreflect.BytesKind:
		fmt.Fprintf(m, "    if ([v%s isKindOfClass:[NSData class]] || v%s == nil) { self.%s = v%s; }\n", field.Pascal, field.Pascal, field.Lower, field.Pascal)
	case protoreflect.BoolKind:
		if field.NeedsHas || field.Oneof != "" {
			fmt.Fprintf(m, "    if (v%s != nil) { self.%s = [v%s boolValue]; }\n", field.Pascal, field.Lower, field.Pascal)
		} else {
			fmt.Fprintf(m, "    self.%s = [v%s boolValue];\n", field.Lower, field.Pascal)
		}
	default:
		if field.NeedsHas || field.Oneof != "" {
			fmt.Fprintf(m, "    if (v%s != nil) { self.%s = (%s)[v%s %s]; }\n", field.Pascal, field.Lower, objcScalar(field), field.Pascal, objcNumberGetter(field))
		} else {
			fmt.Fprintf(m, "    self.%s = (%s)[v%s %s];\n", field.Lower, objcScalar(field), field.Pascal, objcNumberGetter(field))
		}
	}
}

// writeObjcAccessors emits the presence-tracking setters (assigning a value
// marks it present, like the official runtime), the lazy repeated containers,
// the oneof has-bit readers and clearers, and the repeated count readers.
func writeObjcAccessors(m *strings.Builder, f *model.File, msg model.Message) {
	for _, field := range msg.Fields {
		if field.Skip != "" || field.Repeated {
			continue
		}
		if field.Oneof == "" && field.Message == "" && !field.NeedsHas {
			continue
		}
		if field.Oneof != "" {
			writeObjcOneofSetter(m, msg, field)
			continue
		}
		if field.Message != "" {
			fmt.Fprintf(m, "- (void)set%s:(nullable %s)v\n{\n    _%s = v;\n    _has%s = (v != nil);\n}\n\n", field.Pascal, objcScalar(field), field.Lower, field.Pascal)
			continue
		}
		switch field.Kind {
		case protoreflect.StringKind, protoreflect.BytesKind:
			fmt.Fprintf(m, "- (void)set%s:(nullable %s)v\n{\n    _%s = [v copy];\n    _has%s = (v != nil);\n}\n\n", field.Pascal, objcScalar(field), field.Lower, field.Pascal)
		default:
			fmt.Fprintf(m, "- (void)set%s:(%s)v\n{\n    _%s = v;\n    _has%s = YES;\n}\n\n", field.Pascal, objcScalar(field), field.Lower, field.Pascal)
		}
	}
	for _, field := range msg.Fields {
		if field.Skip != "" || !field.Repeated {
			continue
		}
		fmt.Fprintf(m, "- (nullable NSMutableArray<%s> *)%s\n{\n    if (_%s == nil) { _%s = [NSMutableArray array]; }\n    return _%s;\n}\n\n",
			objcElem(field), objcArray(field), objcArray(field), objcArray(field), objcArray(field))
		fmt.Fprintf(m, "- (void)set%s:(nullable NSMutableArray<%s> *)v\n{\n    _%s = [v mutableCopy];\n}\n\n",
			field.Pascal+"Array", objcElem(field), objcArray(field))
		fmt.Fprintf(m, "- (NSUInteger)%s_Count\n{\n    return self.%s.count;\n}\n\n", objcArray(field), objcArray(field))
	}
	for _, o := range objcOneofs(msg) {
		for _, field := range msg.Fields {
			if field.Skip != "" || field.Oneof != o {
				continue
			}
			fmt.Fprintf(m, "- (BOOL)has%s\n{\n    return _%s == %s;\n}\n\n", field.Pascal, objcOneofCaseProp(o), objcOneofValue(msg, field))
		}
		fmt.Fprintf(m, "- (void)clear%s\n{\n", model.Pascal(o))
		for _, field := range msg.Fields {
			if field.Skip != "" || field.Oneof != o {
				continue
			}
			if field.Message != "" || field.Kind == protoreflect.StringKind || field.Kind == protoreflect.BytesKind {
				fmt.Fprintf(m, "    _%s = nil;\n", field.Lower)
			} else {
				fmt.Fprintf(m, "    _%s = 0;\n", field.Lower)
			}
		}
		fmt.Fprintf(m, "    _%s = %s;\n}\n\n", objcOneofCaseProp(o), objcOneofUnset(msg, o))
	}
}

// writeObjcOneofSetter emits one oneof member setter: assigning selects the
// member and clears its siblings (assigning nil clears the whole oneof, like
// the official runtime).
func writeObjcOneofSetter(m *strings.Builder, msg model.Message, field model.Field) {
	null := ""
	if field.Message != "" || field.Kind == protoreflect.StringKind || field.Kind == protoreflect.BytesKind {
		null = "nullable "
	}
	fmt.Fprintf(m, "- (void)set%s:(%s%s)v\n{\n", field.Pascal, null, objcScalar(field))
	for _, sib := range msg.Fields {
		if sib.Skip != "" || sib.Oneof != field.Oneof || sib.Number == field.Number {
			continue
		}
		if sib.Message != "" || sib.Kind == protoreflect.StringKind || sib.Kind == protoreflect.BytesKind {
			fmt.Fprintf(m, "    _%s = nil;\n", sib.Lower)
		} else {
			fmt.Fprintf(m, "    _%s = 0;\n", sib.Lower)
		}
	}
	switch {
	case field.Message != "":
		fmt.Fprintf(m, "    _%s = v;\n    _%s = v != nil ? %s : %s;\n}\n\n",
			field.Lower, objcOneofCaseProp(field.Oneof), objcOneofValue(msg, field), objcOneofUnset(msg, field.Oneof))
	case field.Kind == protoreflect.StringKind || field.Kind == protoreflect.BytesKind:
		fmt.Fprintf(m, "    _%s = [v copy];\n    _%s = v != nil ? %s : %s;\n}\n\n",
			field.Lower, objcOneofCaseProp(field.Oneof), objcOneofValue(msg, field), objcOneofUnset(msg, field.Oneof))
	default:
		fmt.Fprintf(m, "    _%s = v;\n    _%s = %s;\n}\n\n",
			field.Lower, objcOneofCaseProp(field.Oneof), objcOneofValue(msg, field))
	}
}

func objcNumberGetter(field model.Field) string {
	switch field.Kind {
	case protoreflect.FloatKind:
		return "floatValue"
	case protoreflect.DoubleKind:
		return "doubleValue"
	case protoreflect.Int64Kind, protoreflect.Sint64Kind, protoreflect.Sfixed64Kind,
		protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		return "longLongValue"
	default:
		return "intValue"
	}
}
