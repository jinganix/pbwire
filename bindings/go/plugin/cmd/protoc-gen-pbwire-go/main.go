package main

import (
	"fmt"
	"strings"

	"pbwire-go-plugin/internal/core"
	"pbwire-go-plugin/internal/gogen"
	"pbwire-go-plugin/internal/pluginutil"

	pluginpb "google.golang.org/protobuf/types/pluginpb"
)

func main() {
	pluginutil.Run(func(req *pluginpb.CodeGeneratorRequest) ([]*pluginpb.CodeGeneratorResponse_File, error) {
		opts, err := parseOptions(req.GetParameter())
		if err != nil {
			return nil, err
		}
		ctx, err := core.NewRequestContext(req)
		if err != nil {
			return nil, err
		}
		var files []*pluginpb.CodeGeneratorResponse_File
		for _, fd := range ctx.TargetDescriptors {
			name, content, err := gogen.Generate(fd, opts)
			if err != nil {
				return nil, err
			}
			files = append(files, pluginutil.NewFile(name, content))
		}
		return files, nil
	})
}

// parseOptions reads the comma-separated plugin parameter list. The only
// known key is runtime_import=<import path of the pbwire runtime package>.
func parseOptions(param string) (gogen.Options, error) {
	opts := gogen.Options{RuntimeImport: gogen.DefaultRuntimeImport}
	for _, item := range strings.Split(param, ",") {
		if item == "" {
			continue
		}
		key, value, ok := strings.Cut(item, "=")
		if !ok {
			return opts, fmt.Errorf("parameter %q: want key=value", item)
		}
		switch key {
		case "runtime_import":
			if value == "" {
				return opts, fmt.Errorf("parameter runtime_import: empty import path")
			}
			opts.RuntimeImport = value
		default:
			return opts, fmt.Errorf("unknown parameter %q (want runtime_import=<path>)", key)
		}
	}
	return opts, nil
}
