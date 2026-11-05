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
		files = javaFiles(f, opt)
	case "kotlin":
		files = kotlinFiles(f)
	case "csharp":
		files = csharpFiles(f, opt)
	case "dart":
		files = dartFiles(f, opt)
	case "php":
		files = phpFiles(f, opt)
	case "ruby":
		files = rubyFiles(f, opt)
	case "python":
		files = pythonFiles(f, opt)
	case "typescript":
		files = typeScriptFiles(f, opt)
	case "javascript":
		files = javaScriptFiles(f, opt)
	case "rust":
		files = rustFiles(f, opt)
	case "objc":
		files = objcFiles(f, opt)
	case "lua":
		files = luaFiles(f, opt)
	case "go":
		files, err = goFiles(f, opt)
		if err != nil {
			return nil, err
		}
	case "swift":
		files = swiftFiles(f, opt)
	case "zig":
		files = zigFiles(f, opt)
	case "scala":
		files = scalaFiles(f, opt)
	case "clojure":
		files = clojureFiles(f, opt)
	case "fsharp":
		files = fsharpFiles(f, opt)
	case "elixir":
		files = elixirFiles(f)
	case "julia":
		files = juliaFiles(f, opt)
	case "r":
		files = rFiles(f, opt)
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
