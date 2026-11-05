// Translation shim for Zig 0.15+: @cImport was removed, so the build
// feeds this header through translate-c and imports the result as "c".
// It carries no code of its own; every symbol comes from the C core.
#include "pbwire.h"
