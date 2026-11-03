package main

import (
	"pbwire-c-plugin/internal/cgen"
	"pbwire-c-plugin/internal/core"
	"pbwire-c-plugin/internal/pluginutil"

	pluginpb "google.golang.org/protobuf/types/pluginpb"
)

func main() {
	pluginutil.Run(func(req *pluginpb.CodeGeneratorRequest) ([]*pluginpb.CodeGeneratorResponse_File, error) {
		ctx, err := core.NewRequestContext(req)
		if err != nil {
			return nil, err
		}
		var files []*pluginpb.CodeGeneratorResponse_File
		for _, fd := range ctx.TargetDescriptors {
			name, content, err := cgen.Generate(fd)
			if err != nil {
				return nil, err
			}
			files = append(files, pluginutil.NewFile(name, content))
		}
		return files, nil
	})
}
