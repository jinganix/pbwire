package gen

import (
	"encoding/base64"
	"fmt"
	"strings"

	"pbwire-codegen/internal/model"

	"google.golang.org/protobuf/reflect/protoreflect"
)

func luaFiles(f *model.File, opt ...Options) []File {
	// The FileDescriptorSet travels in opt.DescSetB64 (see gen.Generate).
	// The call stays variadic so the old dispatcher luaFiles(f) keeps
	// compiling (and keeps emitting the schema-argument form) until it
	// forwards opt; with the set present the module is self-contained.
	var desc []byte
	if len(opt) > 0 && opt[0].DescSetB64 != "" {
		if raw, err := base64.StdEncoding.DecodeString(opt[0].DescSetB64); err == nil {
			desc = raw
		}
	}
	var b strings.Builder
	b.WriteString(headerP(f, "--", "Generated tables over the pbwire dynamic API: assign fields, encode, decode."))
	if len(desc) > 0 {
		b.WriteString("local pbwire = require(\"pbwire\")\n")
		for _, imp := range f.Imports {
			fmt.Fprintf(&b, "local %s = require(\"%s\")\n", stem(imp.Path), stem(imp.Path))
		}
		b.WriteString("\n")
		b.WriteString("-- The embedded FileDescriptorSet: no .desc file at use time.\n")
		b.WriteString("local DESC = " + luaString(desc) + "\n\n")
		b.WriteString("local _schema = nil\n")
		b.WriteString("local function schema()\n")
		b.WriteString("    if _schema == nil then _schema = pbwire.load(DESC) end\n")
		b.WriteString("    return _schema\n")
		b.WriteString("end\n\n")
	}
	b.WriteString("local M = {}\n\n")
	if len(desc) > 0 {
		b.WriteString("-- The embedded schema, for dynamic-API cross-checks (no .desc file).\n")
		b.WriteString("M.schema = schema\n\n")
	}
	b.WriteString("-- merge overlays init onto dst; a nil init is a fresh default.\n")
	b.WriteString("local function merge(dst, init)\n")
	b.WriteString("    if init ~= nil then\n")
	b.WriteString("        for k, v in pairs(init) do dst[k] = v end\n")
	b.WriteString("    end\n")
	b.WriteString("    return dst\n")
	b.WriteString("end\n\n")
	b.WriteString("-- map_list copies an array through f; a nil array maps to {}.\n")
	b.WriteString("local function map_list(xs, f)\n")
	b.WriteString("    local out = {}\n")
	b.WriteString("    if xs ~= nil then\n")
	b.WriteString("        for i = 1, #xs do out[i] = f(xs[i]) end\n")
	b.WriteString("    end\n")
	b.WriteString("    return out\n")
	b.WriteString("end\n\n")
	b.WriteString("local function identity(v) return v end\n\n")
	for _, e := range f.Enums {
		fmt.Fprintf(&b, "-- %s\n", e.Full)
		fmt.Fprintf(&b, "M.%s = {\n", className(f, e.Name))
		for _, v := range e.Values {
			fmt.Fprintf(&b, "    %s = %d,\n", v.Name, v.Number)
		}
		b.WriteString("}\n\n")
	}
	for _, m := range f.Messages {
		writeLuaMessage(&b, f, m, len(desc) > 0)
	}
	b.WriteString("return M\n")
	return []File{{Name: base(f) + ".lua", Content: b.String()}}
}

// luaMsg names the table for a message field: the sibling module's table
// when foreign (required above), this module's M otherwise.
func luaMsg(f *model.File, field model.Field) string {
	if field.Import != "" {
		if imp := findImport(f, field.Import); imp != nil {
			return stem(imp.Path) + "." + imp.Prefix + "_" + field.Message
		}
	}
	return "M." + className(f, field.Message)
}

func writeLuaMessage(b *strings.Builder, f *model.File, m model.Message, embedded bool) {
	cls := className(f, m.Name)
	fmt.Fprintf(b, "-- %s\n", m.Full)
	fmt.Fprintf(b, "M.%s = {}\n\n", cls)
	fmt.Fprintf(b, "M.%s.new = function(init)\n", cls)
	b.WriteString("    return merge({\n")
	for _, field := range m.Fields {
		if field.Skip != "" {
			fmt.Fprintf(b, "        -- skipped %s: %s\n", field.Name, field.Skip)
			continue
		}
		if field.NeedsHas {
			fmt.Fprintf(b, "        has_%s = false,\n", field.Name)
		}
		if def, ok := luaDefault(field); ok {
			fmt.Fprintf(b, "        %s = %s,\n", field.Name, def)
		}
	}
	b.WriteString("    }, init)\n")
	b.WriteString("end\n\n")

	fmt.Fprintf(b, "M.%s.to_table = function(t)\n", cls)
	b.WriteString("    local out = {}\n")
	for _, field := range m.Fields {
		if field.Skip != "" {
			continue
		}
		name := field.Name
		switch {
		case field.Required:
			// Required fields are always present: a plain assignment, so
			// the generated code reads like hand-written Lua.
			switch {
			case field.Repeated && field.Message != "":
				fmt.Fprintf(b, "    out.%s = map_list(t.%s, %s.to_table)\n", name, name, luaMsg(f, field))
			case field.Repeated:
				fmt.Fprintf(b, "    out.%s = t.%s\n", name, name)
			case field.Message != "":
				fmt.Fprintf(b, "    out.%s = %s.to_table(t.%s)\n", name, luaMsg(f, field), name)
			default:
				fmt.Fprintf(b, "    out.%s = t.%s\n", name, name)
			}
		case field.Repeated && field.Message != "":
			fmt.Fprintf(b, "    if t.%s ~= nil and #t.%s > 0 then out.%s = map_list(t.%s, %s.to_table) end\n",
				name, name, name, name, luaMsg(f, field))
		case field.Repeated:
			fmt.Fprintf(b, "    if t.%s ~= nil and #t.%s > 0 then out.%s = t.%s end\n", name, name, name, name)
		case field.Message != "":
			fmt.Fprintf(b, "    if t.%s ~= nil then out.%s = %s.to_table(t.%s) end\n",
				name, name, luaMsg(f, field), name)
		case field.NeedsHas:
			fmt.Fprintf(b, "    if t.has_%s then out.%s = t.%s end\n", name, name, name)
		case field.Kind == protoreflect.BoolKind:
			fmt.Fprintf(b, "    if t.%s then out.%s = t.%s end\n", name, name, name)
		case field.Kind == protoreflect.StringKind, field.Kind == protoreflect.BytesKind:
			fmt.Fprintf(b, "    if t.%s ~= nil and t.%s ~= \"\" then out.%s = t.%s end\n", name, name, name, name)
		default:
			fmt.Fprintf(b, "    if t.%s ~= nil and t.%s ~= 0 then out.%s = t.%s end\n", name, name, name, name)
		}
	}
	b.WriteString("    return out\n")
	b.WriteString("end\n\n")

	fmt.Fprintf(b, "M.%s.from_table = function(data)\n", cls)
	b.WriteString("    return {\n")
	for _, field := range m.Fields {
		if field.Skip != "" {
			continue
		}
		name := field.Name
		if field.NeedsHas {
			fmt.Fprintf(b, "        has_%s = data.%s ~= nil,\n", name, name)
		}
		fmt.Fprintf(b, "        %s = %s,\n", name, luaFromTable(f, field))
	}
	b.WriteString("    }\n")
	b.WriteString("end\n\n")

	fmt.Fprintf(b, "M.%s.encode = function(", cls)
	if embedded {
		b.WriteString("a, b)\n")
		// One argument encodes through the embedded schema; two keep the
		// old (schema, table) form, so existing callers keep working.
		fmt.Fprintf(b, "    if b ~= nil then return a:encode(%q, M.%s.to_table(b)) end\n", m.Full, cls)
		fmt.Fprintf(b, "    return schema():encode(%q, M.%s.to_table(a))\n", m.Full, cls)
	} else {
		b.WriteString("schema, t)\n")
		fmt.Fprintf(b, "    return schema:encode(%q, M.%s.to_table(t))\n", m.Full, cls)
	}
	b.WriteString("end\n\n")
	fmt.Fprintf(b, "M.%s.decode = function(", cls)
	if embedded {
		b.WriteString("a, b)\n")
		fmt.Fprintf(b, "    if b ~= nil then return M.%s.from_table(a:decode(%q, b)) end\n", cls, m.Full)
		fmt.Fprintf(b, "    return M.%s.from_table(schema():decode(%q, a))\n", cls, m.Full)
	} else {
		b.WriteString("schema, wire)\n")
		fmt.Fprintf(b, "    return M.%s.from_table(schema:decode(%q, wire))\n", cls, m.Full)
	}
	b.WriteString("end\n\n")
}

// luaDefault is the new() value for a field the generated table always carries.
// Message fields and explicit-presence values default to nil (absent key),
// so ok is false for them: the key stays out until assigned.
func luaDefault(field model.Field) (def string, ok bool) {
	if field.Repeated {
		return "{}", true
	}
	if field.Message != "" || field.NeedsHas {
		return "", false
	}
	switch field.Kind {
	case protoreflect.BoolKind:
		return "false", true
	case protoreflect.StringKind, protoreflect.BytesKind:
		return "\"\"", true
	case protoreflect.FloatKind, protoreflect.DoubleKind:
		return "0.0", true
	default:
		return "0", true
	}
}

func luaFromTable(f *model.File, field model.Field) string {
	key := "data." + field.Name
	switch {
	case field.Repeated && field.Message != "":
		return fmt.Sprintf("map_list(%s, %s.from_table)", key, luaMsg(f, field))
	case field.Repeated:
		return fmt.Sprintf("map_list(%s, identity)", key)
	case field.Message != "":
		return fmt.Sprintf("%s ~= nil and %s.from_table(%s) or nil", key, luaMsg(f, field), key)
	case field.NeedsHas:
		return key
	case field.Kind == protoreflect.BoolKind:
		return fmt.Sprintf("%s or false", key)
	case field.Kind == protoreflect.StringKind, field.Kind == protoreflect.BytesKind:
		return fmt.Sprintf("%s or \"\"", key)
	case field.Kind == protoreflect.FloatKind, field.Kind == protoreflect.DoubleKind:
		return fmt.Sprintf("%s or 0.0", key)
	default:
		return fmt.Sprintf("%s or 0", key)
	}
}

// luaString renders raw as a Lua string expression: one decimal escape per
// byte (valid on 5.1 through 5.5 and LuaJIT) inside a table.concat. A flat
// table keeps parsing iterative: a single ".." chain would nest one level
// per chunk and overflow the C stack on large descriptor sets.
func luaString(raw []byte) string {
	const perLine = 256
	var b strings.Builder
	b.WriteString("table.concat({\n")
	for i := 0; i < len(raw); i += perLine {
		end := i + perLine
		if end > len(raw) {
			end = len(raw)
		}
		b.WriteString("    \"")
		for _, v := range raw[i:end] {
			fmt.Fprintf(&b, "\\%03d", v)
		}
		b.WriteString("\",\n")
	}
	b.WriteString("})")
	return b.String()
}
