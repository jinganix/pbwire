package testutil

import (
	"os"
	"path/filepath"
	"runtime"

	"google.golang.org/protobuf/proto"
	pluginpb "google.golang.org/protobuf/types/pluginpb"
)

// RepoRoot is the pbwire checkout root (five levels above this file:
// bindings/go/plugin/internal/testutil -> repo root).
func RepoRoot() string {
	_, file, _, _ := runtime.Caller(0)
	return filepath.Clean(filepath.Join(filepath.Dir(file), "..", "..", "..", "..", ".."))
}

// LoadRequest reads a checked-in CodeGeneratorRequest dump.
func LoadRequest(path string) (*pluginpb.CodeGeneratorRequest, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	req := &pluginpb.CodeGeneratorRequest{}
	if err := proto.Unmarshal(data, req); err != nil {
		return nil, err
	}
	return req, nil
}

// ReadFile reads a file relative to the repo root.
func ReadFile(rel string) (string, error) {
	data, err := os.ReadFile(filepath.Join(RepoRoot(), rel))
	if err != nil {
		return "", err
	}
	return string(data), nil
}
