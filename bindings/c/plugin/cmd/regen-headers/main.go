package main

// regen-headers turns a captured CodeGeneratorRequest (the test.dump a plugin
// test replays) back into the headers it generates, so the pinned fixtures
// under testdata/c/ can be refreshed without protoc at test time:
//
//	regen-headers test.dump <out-dir>
//
// One file is written per generated header, named as protoc would name it.

import (
	"fmt"
	"os"
	"path/filepath"

	"pbwire-c-plugin/internal/cgen"
	"pbwire-c-plugin/internal/core"

	"google.golang.org/protobuf/proto"
	pluginpb "google.golang.org/protobuf/types/pluginpb"
)

func main() {
	if len(os.Args) != 3 {
		fmt.Fprintln(os.Stderr, "usage: regen-headers <test.dump> <out-dir>")
		os.Exit(2)
	}
	raw, err := os.ReadFile(os.Args[1])
	if err != nil {
		panic(err)
	}
	req := &pluginpb.CodeGeneratorRequest{}
	if err := proto.Unmarshal(raw, req); err != nil {
		panic(err)
	}
	ctx, err := core.NewRequestContext(req)
	if err != nil {
		panic(err)
	}
	if err := os.MkdirAll(os.Args[2], 0o755); err != nil {
		panic(err)
	}
	for _, fd := range ctx.Descriptors {
		name, body, err := cgen.Generate(fd)
		if err != nil {
			panic(err)
		}
		if err := os.WriteFile(filepath.Join(os.Args[2], name), []byte(body), 0o644); err != nil {
			panic(err)
		}
	}
}
