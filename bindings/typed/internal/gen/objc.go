package gen

import (
	"fmt"
	"strings"

	"pbwire-typed/internal/model"

	"google.golang.org/protobuf/reflect/protoreflect"
)

func objcScalar(field model.Field) string {
	switch field.Kind {
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind,
		protoreflect.Uint32Kind, protoreflect.Fixed32Kind, protoreflect.EnumKind:
		return "int32_t"
	case protoreflect.Int64Kind, protoreflect.Sint64Kind, protoreflect.Sfixed64Kind,
		protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		return "int64_t"
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

// objcElem is the array element class of a repeated field.
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

// objcFiles emits one header and one implementation per proto file. Repeated
// fields are skipped: the PBWMessage wrapper has no repeated-field API.
func objcFiles(f *model.File) []File {
	var h, m strings.Builder
	guard := "PBWGEN_" + strings.ToUpper(strings.ReplaceAll(base(f), "-", "_")) + "_H"
	h.WriteString(header(f, "Typed Objective-C classes over PBWSchema/PBWMessage."))
	fmt.Fprintf(&h, "#ifndef %s\n#define %s\n\n#import <Foundation/Foundation.h>\n\n#import \"PBWMessage.h\"\n#import \"PBWSchema.h\"\n\nNS_ASSUME_NONNULL_BEGIN\n\n", guard, guard)
	m.WriteString(header(f, "Typed Objective-C classes over PBWSchema/PBWMessage."))
	fmt.Fprintf(&m, "#import %q\n\nNS_ASSUME_NONNULL_BEGIN\n\n", base(f)+".h")

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

func objcEnum(local string) string { return "PBWGen_" + local }

func writeObjcMessage(h, m *strings.Builder, f *model.File, msg model.Message) {
	cls := objcClass(msg.Name)
	fmt.Fprintf(h, "/** %s */\n@interface %s : NSObject\n", msg.Full, cls)
	for _, field := range msg.Fields {
		if field.Skip != "" {
			fmt.Fprintf(h, "// skipped %s: %s\n", field.Name, field.Skip)
			continue
		}
		if field.Repeated {
			fmt.Fprintf(h, "@property (nonatomic, copy, nullable) NSArray<%s> *%s;\n", objcElem(field), field.Lower)
			continue
		}
		if field.NeedsHas {
			fmt.Fprintf(h, "@property (nonatomic, assign) BOOL has%s;\n", field.Pascal)
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
	}
	fmt.Fprintf(h, "- (BOOL)encodeInto:(PBWMessage *)msg error:(NSError **)error;\n")
	fmt.Fprintf(h, "- (nullable NSData *)encodeWithSchema:(PBWSchema *)schema error:(NSError **)error;\n")
	fmt.Fprintf(h, "+ (nullable instancetype)decodeWithSchema:(PBWSchema *)schema data:(NSData *)data error:(NSError **)error;\n")
	fmt.Fprintf(h, "+ (instancetype)fromMessage:(PBWMessage *)msg;\n@end\n\n")

	fmt.Fprintf(m, "@implementation %s\n\n", cls)
	fmt.Fprintf(m, "- (BOOL)encodeInto:(PBWMessage *)msg error:(NSError **)error\n{\n")
	for _, field := range msg.Fields {
		if field.Skip != "" {
			continue
		}
		if field.Repeated {
			fmt.Fprintf(m, "    if (self.%s.count > 0) {\n", field.Lower)
			if field.Message != "" {
				fmt.Fprintf(m, "        for (%s v in self.%s) {\n", objcClass(field.Message)+" *", field.Lower)
				fmt.Fprintf(m, "            PBWMessage *child = [msg addMessageForField:@%q error:error];\n", field.Name)
				m.WriteString("            if (child == nil || ![v encodeInto:child error:error]) { return NO; }\n        }\n")
			} else {
				fmt.Fprintf(m, "        for (%s v in self.%s) {\n", objcElem(field), field.Lower)
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
	m.WriteString("    return YES;\n}\n\n")

	fmt.Fprintf(m, "- (nullable NSData *)encodeWithSchema:(PBWSchema *)schema error:(NSError **)error\n{\n")
	fmt.Fprintf(m, "    PBWMessage *msg = [schema messageByName:@%q];\n", msg.Full)
	m.WriteString("    if (msg == nil || ![self encodeInto:msg error:error]) { return nil; }\n    return [msg encode:error];\n}\n\n")

	fmt.Fprintf(m, "+ (nullable instancetype)decodeWithSchema:(PBWSchema *)schema data:(NSData *)data error:(NSError **)error\n{\n")
	fmt.Fprintf(m, "    PBWMessage *msg = [schema messageByName:@%q];\n", msg.Full)
	m.WriteString("    if (msg == nil) { return nil; }\n    PBWMessage *decoded = [msg decode:data error:error];\n    return decoded == nil ? nil : [self fromMessage:decoded];\n}\n\n")

	fmt.Fprintf(m, "+ (instancetype)fromMessage:(PBWMessage *)msg\n{\n    %s *o = [[self alloc] init];\n", cls)
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
			fmt.Fprintf(m, "        }\n        o.%s = a%s;\n    }\n", field.Lower, field.Pascal)
			continue
		}
		writeObjcRead(m, f, field)
	}
	m.WriteString("    return o;\n}\n\n@end\n\n")
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
		fmt.Fprintf(m, "    if (c%s != nil) { o.%s = [%s fromMessage:c%s]; }\n", field.Pascal, field.Lower, objcClass(field.Message), field.Pascal)
		return
	}
	fmt.Fprintf(m, "    id v%s = [msg valueForField:@%q];\n", field.Pascal, field.Name)
	if field.NeedsHas {
		fmt.Fprintf(m, "    o.has%s = v%s != nil;\n", field.Pascal, field.Pascal)
	}
	switch field.Kind {
	case protoreflect.StringKind:
		fmt.Fprintf(m, "    o.%s = [v%s isKindOfClass:[NSString class]] ? v%s : nil;\n", field.Lower, field.Pascal, field.Pascal)
	case protoreflect.BytesKind:
		fmt.Fprintf(m, "    o.%s = [v%s isKindOfClass:[NSData class]] ? v%s : nil;\n", field.Lower, field.Pascal, field.Pascal)
	case protoreflect.BoolKind:
		fmt.Fprintf(m, "    o.%s = [v%s boolValue];\n", field.Lower, field.Pascal)
	default:
		fmt.Fprintf(m, "    o.%s = (%s)[v%s %s];\n", field.Lower, objcScalar(field), field.Pascal, objcNumberGetter(field))
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
