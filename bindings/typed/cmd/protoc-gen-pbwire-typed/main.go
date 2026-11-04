// Command protoc-gen-pbwire-typed generates typed message definitions for the
// pbwire bindings. Typed fields give the host language's compiler and IDE
// something to check; encode/decode delegate to the binding's dynamic API, so
// the wire format keeps exactly one implementation (the C core).
//
//	protoc --plugin=protoc-gen-pbwire-typed=... \
//	    --pbwire-typed_out=. --pbwire-typed_opt=lang=java demo.proto
//
// Options: lang=java|kotlin|csharp|dart|php|ruby|python|typescript|javascript|rust|objc,
// prefix=<ClassPrefix> (default: CamelCase of the proto package) and
// dir=<subdir> (place the generated files under a subdirectory).
package main

import (
	"fmt"
	"strings"

	"pbwire-typed/internal/core"
	"pbwire-typed/internal/gen"
	"pbwire-typed/internal/pluginutil"

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
			out, err := gen.Generate(fd, opt)
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

func parseOptions(param string) (gen.Options, error) {
	var opt gen.Options
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
		default:
			return opt, fmt.Errorf("unknown option %q", key)
		}
	}
	if opt.Lang == "" {
		return opt, fmt.Errorf("missing lang= (java, kotlin, csharp, dart, php, ruby, python, typescript, javascript, rust, objc)")
	}
	return opt, nil
}
