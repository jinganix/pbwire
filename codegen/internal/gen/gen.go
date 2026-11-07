// Package gen turns the neutral model into one language's generated message
// definitions. Every emitter is a text template over model.File: typed fields
// plus encode/decode that call the binding's existing dynamic API, so the wire
// format still has exactly one implementation (the C core).
package gen

import (
	"fmt"
	"strings"

	"pbwire-codegen/internal/model"

	"google.golang.org/protobuf/reflect/protoreflect"
)

// Options comes from the protoc plugin parameter: lang=java,prefix=Foo,dir=gen.
type Options struct {
	Lang   string
	Prefix string // class prefix override; empty means derive from the package
	Dir    string // output subdirectory inside the protoc output tree
	// Model selects the message implementation files (message + Builder).
	// Off leaves the read interfaces plus the codec tables: a hand-written
	// class with the canonical shape shares the rows through forBuilder.
	Model bool
	// Register selects the static self-registration block in each message:
	// off leaves registration to an explicit register call (deferred or
	// custom Builder), failing loudly until then.
	Register bool
	// Registry selects the whole-file message list (<Prefix>_Registry):
	// pure data (class literals, no init), so referencing it costs nothing.
	// Schema.fromClasses(<Prefix>_Registry.MESSAGES) is whole-schema
	// registration with no descriptor file. Needs Model.
	Registry bool
	// DescSetB64 is the FileDescriptorSet for this file (transitive closure
	// of its imports) encoded as base64. Emitters whose runtime embeds its
	// own schema (no .desc file at use time) read it from here.
	DescSetB64 string
	// Mappings overrides sibling import paths per .proto file, protoc-gen-go
	// style: Mdep.proto=example.com/mod/gen/dep. Only languages whose module
	// system needs an explicit path (Go) read it; the rest import the sibling
	// generated file by name.
	Mappings map[string]string
}

// File is one generated file.
type File struct {
	Name    string
	Content string
}

// Generate dispatches to the emitter for opt.Lang.
func Generate(fd protoreflect.FileDescriptor, opt Options) ([]File, error) {
	f := model.Build(fd)
	if opt.Prefix != "" {
		f.Prefix = opt.Prefix
	}
	var files []File
	var err error
	switch opt.Lang {
	case "java":
		files, err = javaFiles(f, opt)
		if err != nil {
			return nil, err
		}
	case "kotlin":
		files = kotlinFiles(f)
	case "csharp":
		files, err = csharpFiles(f, opt)
		if err != nil {
			return nil, err
		}
	case "dart":
		files, err = dartFiles(f, opt)
		if err != nil {
			return nil, err
		}
	case "php":
		files, err = phpFiles(f, opt)
		if err != nil {
			return nil, err
		}
	case "ruby":
		files, err = rubyFiles(f, opt)
		if err != nil {
			return nil, err
		}
	case "python":
		files, err = pythonFiles(f, opt)
		if err != nil {
			return nil, err
		}
	case "typescript":
		files = typeScriptFiles(f, opt)
	case "javascript":
		files = javaScriptFiles(f, opt)
	case "rust":
		files = rustFiles(f, opt)
	case "objc":
		files, err = objcFiles(f, opt)
		if err != nil {
			return nil, err
		}
	case "lua":
		files, err = luaFiles(f, opt)
		if err != nil {
			return nil, err
		}
	case "go":
		files, err = goFiles(f, opt)
		if err != nil {
			return nil, err
		}
	case "swift":
		files, err = swiftFiles(f, opt)
		if err != nil {
			return nil, err
		}
	case "zig":
		files = zigFiles(f, opt)
	case "scala":
		files, err = scalaFiles(f, opt)
		if err != nil {
			return nil, err
		}
	case "clojure":
		files, err = clojureFiles(f, opt)
		if err != nil {
			return nil, err
		}
	case "fsharp":
		files, err = fsharpFiles(f, opt)
		if err != nil {
			return nil, err
		}
	case "elixir":
		files, err = elixirFiles(f, opt)
		if err != nil {
			return nil, err
		}
	case "julia":
		files, err = juliaFiles(f, opt)
		if err != nil {
			return nil, err
		}
	case "r":
		files, err = rFiles(f, opt)
		if err != nil {
			return nil, err
		}
	default:
		return nil, fmt.Errorf("unknown lang=%q (want java, kotlin, csharp, dart, php, ruby, python, typescript, javascript, rust, objc, lua, go, swift, zig, scala, clojure, fsharp, elixir, julia or r)", opt.Lang)
	}
	if opt.Dir == "" {
		return files, nil
	}
	for i := range files {
		files[i].Name = strings.TrimSuffix(opt.Dir, "/") + "/" + files[i].Name
	}
	return files, nil
}

// base is the file name without extension, used for generated file names.
func base(f *model.File) string { return stem(f.Path) }

// stem maps a .proto path to its output stem (no directory, no extension).
// Sibling imports resolve through it: the generated file for dep.proto in
// the same directory is stem(dep.proto) plus the language's suffix.
func stem(path string) string {
	p := path
	if i := strings.LastIndex(p, "/"); i >= 0 {
		p = p[i+1:]
	}
	return strings.TrimSuffix(p, ".proto")
}

// ident sanitizes a name derived from a file path for hosts that need a plain
// identifier: `my-file.proto` must not emit `import my-file_pbwire`. Bytes
// outside [A-Za-z0-9_] become '_' and a leading digit gets a '_' prefix.
func ident(name string) string {
	var b strings.Builder

	for _, r := range name {
		if (r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z') ||
			(r >= '0' && r <= '9') || r == '_' {
			b.WriteRune(r)
		} else {
			b.WriteRune('_')
		}
	}
	s := b.String()
	if s == "" {
		return "_"
	}
	if s[0] >= '0' && s[0] <= '9' {
		return "_" + s
	}
	return s
}

// foreignMessageNames lists the distinct message types referenced from one
// import, in first-use order. Emitters whose registry is name-keyed (Node)
// import exactly these names from the sibling file.
func foreignMessageNames(f *model.File, path string) []string {
	var names []string
	seen := map[string]bool{}
	for _, m := range f.Messages {
		for _, fl := range m.Fields {
			if fl.Import == path && fl.Message != "" && !seen[fl.Message] {
				seen[fl.Message] = true
				names = append(names, fl.Message)
			}
		}
	}
	return names
}

// findImport returns the Imports entry for a .proto path, or nil.
func findImport(f *model.File, path string) *model.Import {
	for i := range f.Imports {
		if f.Imports[i].Path == path {
			return &f.Imports[i]
		}
	}
	return nil
}

// qualifyFields rewrites foreign message refs in place: every field with
// Import set gets qual(import, local) as its Message, so the emitter's
// existing use sites print the qualified reference untouched. Local fields
// keep their names.
func qualifyFields(f *model.File, qual func(model.Import, string) string) {
	for i := range f.Messages {
		for j := range f.Messages[i].Fields {
			fl := &f.Messages[i].Fields[j]
			if fl.Import == "" || fl.Message == "" {
				continue
			}
			if imp := findImport(f, fl.Import); imp != nil {
				fl.Message = qual(*imp, fl.Message)
			}
		}
	}
}

// className is the emitter's top-level name for a message or enum: the package
// prefix keeps a single global namespace unique.
func className(f *model.File, local string) string { return f.Prefix + "_" + local }

func header(f *model.File, comment string) string { return headerP(f, "//", comment) }

// headerP is header with a language's line comment prefix (# for Ruby/Python).
func headerP(f *model.File, prefix, comment string) string {
	return fmt.Sprintf("%s Generated by protoc-gen-pbwire from %s.\n%s %s\n%s Do not edit; regenerate from the .proto instead.\n\n",
		prefix, f.Path, prefix, comment, prefix)
}
