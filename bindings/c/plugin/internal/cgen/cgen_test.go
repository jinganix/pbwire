package cgen_test

import (
	"os"
	"os/exec"
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
// TestRecurHeadersCompile proves the shapes the pinned headers miss: a
// recursive message, a forward reference, and a member typed from another
// file. All three need a declaration that only exists after the fix (a
// forward typedef, and a table emitted before the table that names it).
func TestRecurHeadersCompile(t *testing.T) {
	cc, err := exec.LookPath("cc")
	if err != nil {
		t.Skip("cc not installed")
	}
	root := testutil.RepoRoot()
	req, err := testutil.LoadRequest(filepath.Join(root, "bindings", "c", "plugin",
		"testdata", "c", "recur", "test.dump"))
	if err != nil {
		t.Fatal(err)
	}
	ctx, err := core.NewRequestContext(req)
	if err != nil {
		t.Fatal(err)
	}
	dir := t.TempDir()
	for _, fd := range ctx.Descriptors {
		name, out, err := cgen.Generate(fd)
		if err != nil {
			t.Fatalf("%s: %v", fd.Path(), err)
		}
		if err := os.WriteFile(filepath.Join(dir, name), []byte(out), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	cmd := exec.Command(cc, "-fsyntax-only", "-std=c99", "-Wall", "-Wextra", "-Werror",
		"-pedantic", "-I", dir, "-I", filepath.Join(root, "bindings", "c", "runtime"),
		"-I", filepath.Join(root, "include"), filepath.Join(dir, "recur.h"))
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("generated header does not compile: %v\n%s", err, out)
	}
}

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
		{"extensiontest", "extensiontest.h", filepath.Join("bindings", "c", "tests", "gen", "extensiontest.h")},
		/* Self-reference, forward reference, and a type from another file:
		 * none of the pinned headers above exercise those shapes. */
		{"recur", "recur.h", filepath.Join("bindings", "c", "plugin", "testdata", "c", "recur", "recur.h")},
		{"recur", "dep.h", filepath.Join("bindings", "c", "plugin", "testdata", "c", "recur", "dep.h")},
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
			/* The fixture may generate more than one header (a cross-file
			 * type); compare the one this case names. */
			var got string
			for _, fd := range ctx.Descriptors {
				n, body, err := cgen.Generate(fd)
				if err != nil {
					t.Fatalf("%s: %v", fd.Path(), err)
				}
				if n == tc.name {
					got = body
				}
			}
			if got == "" {
				t.Fatalf("%s: no generated file named %q", tc.dump, tc.name)
			}
			if err != nil {
				t.Fatal(err)
			}
			want, err := testutil.ReadFile(tc.want)
			if err != nil {
				t.Fatal(err)
			}
			if !testutil.GoldenEqual(got, want) {
				t.Fatalf("generated output differs from %s; run `make -C bindings/c/plugin regen-recur` and inspect the diff", tc.want)
			}
		})
	}
}
