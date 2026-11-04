package gen

import (
	"fmt"
	"strings"

	"pbwire-typed/internal/model"

	"google.golang.org/protobuf/reflect/protoreflect"
)

func luaFiles(f *model.File) []File {
	var b strings.Builder
	b.WriteString(headerP(f, "--", "Typed tables over the pbwire dynamic API: assign fields, encode, decode."))
	b.WriteString("local M = {}\n\n")
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
		writeLuaMessage(&b, f, m)
	}
	b.WriteString("return M\n")
	return []File{{Name: base(f) + ".lua", Content: b.String()}}
}

func writeLuaMessage(b *strings.Builder, f *model.File, m model.Message) {
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
				fmt.Fprintf(b, "    out.%s = map_list(t.%s, M.%s.to_table)\n", name, name, className(f, field.Message))
			case field.Repeated:
				fmt.Fprintf(b, "    out.%s = t.%s\n", name, name)
			case field.Message != "":
				fmt.Fprintf(b, "    out.%s = M.%s.to_table(t.%s)\n", name, className(f, field.Message), name)
			default:
				fmt.Fprintf(b, "    out.%s = t.%s\n", name, name)
			}
		case field.Repeated && field.Message != "":
			fmt.Fprintf(b, "    if t.%s ~= nil and #t.%s > 0 then out.%s = map_list(t.%s, M.%s.to_table) end\n",
				name, name, name, name, className(f, field.Message))
		case field.Repeated:
			fmt.Fprintf(b, "    if t.%s ~= nil and #t.%s > 0 then out.%s = t.%s end\n", name, name, name, name)
		case field.Message != "":
			fmt.Fprintf(b, "    if t.%s ~= nil then out.%s = M.%s.to_table(t.%s) end\n",
				name, name, className(f, field.Message), name)
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

	fmt.Fprintf(b, "M.%s.encode = function(schema, t)\n", cls)
	fmt.Fprintf(b, "    return schema:encode(%q, M.%s.to_table(t))\n", m.Full, cls)
	b.WriteString("end\n\n")
	fmt.Fprintf(b, "M.%s.decode = function(schema, wire)\n", cls)
	fmt.Fprintf(b, "    return M.%s.from_table(schema:decode(%q, wire))\n", cls, m.Full)
	b.WriteString("end\n\n")
}

// luaDefault is the new() value for a field the typed table always carries.
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
		return fmt.Sprintf("map_list(%s, M.%s.from_table)", key, className(f, field.Message))
	case field.Repeated:
		return fmt.Sprintf("map_list(%s, identity)", key)
	case field.Message != "":
		return fmt.Sprintf("%s ~= nil and M.%s.from_table(%s) or nil", key, className(f, field.Message), key)
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
