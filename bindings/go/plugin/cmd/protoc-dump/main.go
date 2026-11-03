package main

import (
	"io"
	"os"

	"google.golang.org/protobuf/proto"
	pluginpb "google.golang.org/protobuf/types/pluginpb"
)

// protoc-dump captures the CodeGeneratorRequest so plugin tests run without
// protoc: `protoc --plugin=protoc-gen-dump=./protoc-dump --dump_out=. a.proto`
// writes test.dump, which the golden tests replay.
func main() {
	data, err := io.ReadAll(os.Stdin)
	if err != nil {
		panic(err)
	}
	req := &pluginpb.CodeGeneratorRequest{}
	if err := proto.Unmarshal(data, req); err != nil {
		panic(err)
	}
	content := string(data)
	resp := &pluginpb.CodeGeneratorResponse{
		File: []*pluginpb.CodeGeneratorResponse_File{
			{
				Name:    proto.String("test.dump"),
				Content: &content,
			},
		},
		// Same as the real plugin: without this protoc withholds
		// proto3 files with optional fields.
		SupportedFeatures: proto.Uint64(uint64(pluginpb.CodeGeneratorResponse_FEATURE_PROTO3_OPTIONAL)),
	}
	out, err := proto.Marshal(resp)
	if err != nil {
		panic(err)
	}
	if _, err := os.Stdout.Write(out); err != nil {
		panic(err)
	}
}
