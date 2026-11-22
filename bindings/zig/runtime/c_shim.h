// Translation shim for Zig 0.15+: @cImport was removed, so the build
// feeds this header through translate-c and imports the result as "c".
// It carries no code of its own; every symbol comes from the C core.
//
// The layout codec (`bindings/c/runtime/pbw_struct.h`) is a header-only ABI
// here: its implementation compiles into the same static archive (build.zig),
// so the generated extern-struct mirror crosses once per op.
#include "pbwire.h"
#include "pbw_struct.h"
