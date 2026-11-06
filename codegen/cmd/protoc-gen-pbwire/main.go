// Command protoc-gen-pbwire generates message definitions for the
// pbwire bindings. Typed fields give the host language's compiler and IDE
// something to check; encode/decode delegate to the binding's dynamic API, so
// the wire format keeps exactly one implementation (the C core).
//
//	protoc --plugin=protoc-gen-pbwire=... \
//	    --pbwire_out=. --pbwire_opt=lang=java demo.proto
//
// Options: lang=java|kotlin|csharp|dart|php|ruby|python|typescript|javascript|rust|objc|lua|go|swift|zig|scala|clojure|fsharp|elixir|julia|r,
// prefix=<ClassPrefix> (default: CamelCase of the proto package),
// dir=<subdir> (place the generated files under a subdirectory),
// model=<true|false> (default true: emit the message implementation files;
// false leaves only the schema sidecar for dynamic-only users),
// register=<true|false> (default true: load the embedded schema lazily on
// first use; false leaves it to an explicit init/use_schema call, failing
// loudly otherwise) and registry=<true|false> (default true: emit the
// whole-file message list plus a preload that warms every type; pure data,
// no runtime cost) and
// M<path>=<importpath> (sibling import override, protoc-gen-go style;
// only languages with explicit module paths read it).
//
// Per-language notes: elixir has no embedded descriptor set (the modules are
// the schema), so there is no second file and register is a no-op — loading
// stays explicit via Pbwire.load_schema/1; r keeps one dotted holder per file
// instead of the old single shared global.
package main

import (
	"encoding/base64"
	"fmt"
	"strconv"
	"strings"

	"pbwire-codegen/internal/core"
	"pbwire-codegen/internal/gen"
	"pbwire-codegen/internal/pluginutil"

	"google.golang.org/protobuf/proto"
	descriptorpb "google.golang.org/protobuf/types/descriptorpb"
	pluginpb "google.golang.org/protobuf/types/pluginpb"
)

func main() {
	pluginutil.Run(func(req *pluginpb.CodeGeneratorRequest) ([]*pluginpb.CodeGeneratorResponse_File, error) {
		opt, err := parseOptions(req.GetParameter())
		if err != nil {
			return nil, err
		}
		ctx, err := core.NewRequestContext(req)
		if err != nil {
			return nil, err
		}
		var files []*pluginpb.CodeGeneratorResponse_File
		for _, fd := range ctx.TargetDescriptors {
			// The embedded schema is this file plus its transitive imports,
			// in protoc order (already topological): exactly what a
			// `protoc --descriptor_set_out` for this file would contain.
			set, err := closureSet(req, fd.Path())
			if err != nil {
				return nil, err
			}
			fileOpt := opt
			fileOpt.DescSetB64 = set
			out, err := gen.Generate(fd, fileOpt)
			if err != nil {
				return nil, err
			}
			for _, file := range out {
				files = append(files, pluginutil.NewFile(file.Name, file.Content))
			}
		}
		return files, nil
	})
}

// closureSet marshals the transitive import closure of path (from the raw
// request protos, protoc order) as a base64 FileDescriptorSet.
func closureSet(req *pluginpb.CodeGeneratorRequest, path string) (string, error) {
	byName := make(map[string]*descriptorpb.FileDescriptorProto, len(req.ProtoFile))
	for _, pb := range req.ProtoFile {
		byName[pb.GetName()] = pb
	}
	var order []string
	seen := map[string]bool{}
	var visit func(name string) error
	visit = func(name string) error {
		if seen[name] {
			return nil
		}
		seen[name] = true
		pb := byName[name]
		if pb == nil {
			return fmt.Errorf("descriptor for %q not in request", name)
		}
		for _, dep := range pb.Dependency {
			if err := visit(dep); err != nil {
				return err
			}
		}
		order = append(order, name)
		return nil
	}
	if err := visit(path); err != nil {
		return "", err
	}
	set := &descriptorpb.FileDescriptorSet{}
	for _, name := range order {
		set.File = append(set.File, byName[name])
	}
	raw, err := proto.Marshal(set)
	if err != nil {
		return "", err
	}
	return base64.StdEncoding.EncodeToString(raw), nil
}

func parseOptions(param string) (gen.Options, error) {
	var opt gen.Options
	// The shipping shape: implementation models that self-register, plus the
	// whole-file list for whole-schema registration.
	opt.Model, opt.Register, opt.Registry = true, true, true
	for _, part := range strings.Split(param, ",") {
		if part == "" {
			continue
		}
		key, value, ok := strings.Cut(part, "=")
		if !ok {
			return opt, fmt.Errorf("bad option %q (want key=value)", part)
		}
		switch key {
		case "lang":
			opt.Lang = value
		case "prefix":
			opt.Prefix = value
		case "dir":
			opt.Dir = value
		case "model":
			b, err := strconv.ParseBool(value)
			if err != nil {
				return opt, fmt.Errorf("bad option %q (want true or false)", part)
			}
			opt.Model = b
		case "register":
			b, err := strconv.ParseBool(value)
			if err != nil {
				return opt, fmt.Errorf("bad option %q (want true or false)", part)
			}
			opt.Register = b
		case "registry":
			b, err := strconv.ParseBool(value)
			if err != nil {
				return opt, fmt.Errorf("bad option %q (want true or false)", part)
			}
			opt.Registry = b
		default:
			if rest, ok := strings.CutPrefix(key, "M"); ok {
				if opt.Mappings == nil {
					opt.Mappings = map[string]string{}
				}
				opt.Mappings[rest] = value
				continue
			}
			return opt, fmt.Errorf("unknown option %q", key)
		}
	}
	if opt.Lang == "" {
		return opt, fmt.Errorf("missing lang= (java, kotlin, csharp, dart, php, ruby, python, typescript, javascript, rust, objc, lua, go, swift, zig, scala, clojure, fsharp, elixir, julia or r)")
	}
	return opt, nil
}
