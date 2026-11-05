package cgen_test

import (
	"path/filepath"
	"testing"

	"pbwire-c-plugin/internal/cgen"
	"pbwire-c-plugin/internal/core"
	"pbwire-c-plugin/internal/testutil"
)

// Each golden file is a pinned generated header the C example/tests build
// against. Generator output must match it byte for byte; the addressbook
// header also proves the proto3-optional and implicit-enum fixes, via the
// matrix protos that need both.
func TestGoldens(t *testing.T) {
	cases := []struct {
		dump string // testdata dir holding the replayed test.dump
		name string // expected output file name
		want string // pinned header, relative to the repo root
	}{
		{"addressbook", "addressbook.h", filepath.Join("bindings", "c", "examples", "gen", "addressbook.h")},
		{"matrix3", "matrix3.h", filepath.Join("bindings", "c", "tests", "gen", "matrix3.h")},
		{"matrix2", "matrix2.h", filepath.Join("bindings", "c", "tests", "gen", "matrix2.h")},
		{"maptest", "maptest.h", filepath.Join("bindings", "c", "tests", "gen", "maptest.h")},
		{"grouptest", "grouptest.h", filepath.Join("bindings", "c", "tests", "gen", "grouptest.h")},
		{"oneoftest", "oneoftest.h", filepath.Join("bindings", "c", "tests", "gen", "oneoftest.h")},
	}
	for _, tc := range cases {
		t.Run(tc.dump, func(t *testing.T) {
			req, err := testutil.LoadRequest(filepath.Join(testutil.RepoRoot(),
				"bindings", "c", "plugin", "testdata", "c", tc.dump, "test.dump"))
			if err != nil {
				t.Fatal(err)
			}
			ctx, err := core.NewRequestContext(req)
			if err != nil {
				t.Fatal(err)
			}
			if len(ctx.TargetDescriptors) != 1 {
				t.Fatalf("targets = %d, want 1", len(ctx.TargetDescriptors))
			}
			name, got, err := cgen.Generate(ctx.TargetDescriptors[0])
			if err != nil {
				t.Fatal(err)
			}
			if name != tc.name {
				t.Fatalf("name = %q, want %q", name, tc.name)
			}
			want, err := testutil.ReadFile(tc.want)
			if err != nil {
				t.Fatal(err)
			}
			if !testutil.GoldenEqual(got, want) {
				t.Fatalf("generated output differs from %s; run `make -C bindings/c/plugin regen` and inspect the diff", tc.want)
			}
		})
	}
}
