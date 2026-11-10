#!/usr/bin/env bash
# Footprint of the codec objects a Cortex-M firmware would link: compile them
# with the ARM toolchain at -Os and sum text/data/bss per module. No link and
# no libc, so the number is the codec's own cost; the firmware's libc, its own
# allocator and the schema data are on top of it.
set -uo pipefail

CC=${MCU_CC:-arm-none-eabi-gcc}
SIZER=${MCU_SIZE:-arm-none-eabi-size}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${MCU_OUT:-"$ROOT/build/mcu"}

if ! command -v "$CC" >/dev/null 2>&1; then
    echo "mcu-size: $CC not found (install arm-none-eabi-gcc, e.g. brew install arm-none-eabi-gcc)" >&2
    exit 1
fi

rm -rf "$OUT"
mkdir -p "$OUT/fsinclude"
FLAGS="-Os -std=c99 -ffreestanding -mthumb -ffunction-sections -fdata-sections -I$OUT/fsinclude -I$ROOT/include -I$ROOT/src"

# A freestanding toolchain ships no libc headers. Only declarations are needed
# to compile, and nothing is linked, so these stubs are enough: the point is the
# codec's own object code, not the firmware's libc.
cat >"$OUT/fsinclude/string.h" <<'EOF'
#include <stddef.h>
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
void *memset(void *, int, size_t);
int memcmp(const void *, const void *, size_t);
size_t strlen(const char *);
int strcmp(const char *, const char *);
int strncmp(const char *, const char *, size_t);
char *strchr(const char *, int);
char *strrchr(const char *, int);
char *strstr(const char *, const char *);
EOF
cat >"$OUT/fsinclude/stdlib.h" <<'EOF'
#include <stddef.h>
void *malloc(size_t);
void *calloc(size_t, size_t);
void *realloc(void *, size_t);
void free(void *);
double strtod(const char *, char **);
void qsort(void *, size_t, size_t, int (*)(const void *, const void *));
void *bsearch(const void *, const void *, size_t, size_t, int (*)(const void *, const void *));
EOF
cat >"$OUT/fsinclude/stdio.h" <<'EOF'
#include <stddef.h>
#include <stdarg.h>
int snprintf(char *, size_t, const char *, ...);
int vsnprintf(char *, size_t, const char *, va_list);
EOF
cat >"$OUT/fsinclude/math.h" <<'EOF'
double floor(double);
#define NAN (0.0/0.0)
#define INFINITY (1.0/0.0)
#define HUGE_VAL (1.0/0.0)
#define isnan(x) 0
#define isinf(x) 0
#define isfinite(x) 1
EOF
cat >"$OUT/fsinclude/ctype.h" <<'EOF'
int tolower(int);
int toupper(int);
EOF
cat >"$OUT/fsinclude/locale.h" <<'EOF'
struct lconv { char *decimal_point; };
struct lconv *localeconv(void);
char *setlocale(int, const char *);
#define LC_ALL 0
#define LC_NUMERIC 1
EOF

for src in "$ROOT"/src/*.c; do
    "$CC" $FLAGS -c -o "$OUT/$(basename "${src%.c}").o" "$src" || exit 1
done
"$CC" $FLAGS -I"$ROOT/bindings/embedded/runtime" -c -o "$OUT/pbwe.o" \
    "$ROOT/bindings/embedded/runtime/pbwe.c" || exit 1

# text data bss dec hex filename; the core set is what decode/encode need, the
# extras are optional modules a firmware links only when it calls them.
"$SIZER" "$OUT"/*.o | awk '
BEGIN {
    split("wire arena ctx schema message descriptor_set encode decode pbwe", core, " ")
    split("json text stream wkt", extra, " ")
    for (i = 1; i <= 4; i++) opt[extra[i]] = 1
}
$1 ~ /^[0-9]+$/ && NF == 6 {
    name = $6
    sub(/^.*\//, "", name)
    sub(/\.o$/, "", name)
    text[name] += $1
    data[name] += $2
    bss[name] += $3
    if (name in opt) { etext += $1; edata += $2; ebss += $3 } else { ctext += $1; cdata += $2; cbss += $3 }
}
END {
    printf "%-16s %8s %8s %8s\n", "object", "text", "data", "bss"
    for (i = 1; i <= 9; i++) {
        n = core[i]
        printf "%-16s %8d %8d %8d\n", n, text[n], data[n], bss[n]
    }
    printf "%-16s %8d %8d %8d\n", "codec total", ctext, cdata, cbss
    printf "%-16s %8s %8s %8s\n", "optional", "text", "data", "bss"
    for (i = 1; i <= 4; i++) {
        n = extra[i]
        printf "%-16s %8d %8d %8d\n", n, text[n], data[n], bss[n]
    }
    printf "%-16s %8d %8d %8d\n", "codec + optional", ctext + etext, cdata + edata, cbss + ebss
}' 

# Optional comparison: the same message through nanopb (runtime + its generated
# code), same flags. `make embedded-size` generates the file and passes the paths.
if [ -n "${NANOPB_DIR:-}" ] && [ -f "${NANOPB_GEN:-}" ]; then
    NBOUT="$OUT/nanopb"
    rm -rf "$NBOUT"
    mkdir -p "$NBOUT"
    GEN=$(cd "$(dirname "$NANOPB_GEN")" && pwd)
    for src in "$NANOPB_DIR/pb_common.c" "$NANOPB_DIR/pb_encode.c" \
        "$NANOPB_DIR/pb_decode.c" "$NANOPB_GEN"; do
        "$CC" $FLAGS -I"$NANOPB_DIR" -I"$GEN" -c -o "$NBOUT/$(basename "${src%.c}").o" \
            "$src" || exit 1
    done
    echo
    echo "nanopb, same message (runtime + its generated code):"
    "$SIZER" "$NBOUT"/*.o | awk '
BEGIN { split("pb_common pb_encode pb_decode msg.pb", nm, " ") }
$1 ~ /^[0-9]+$/ && NF == 6 {
    n = $6
    sub(/^.*\//, "", n)
    sub(/\.o$/, "", n)
    text[n] += $1
    total += $1
    bss[n] += $3
    tbss += $3
}
END {
    printf "%-16s %8s %8s\n", "object", "text", "bss"
    for (i = 1; i <= 4; i++) {
        n = nm[i]
        printf "%-16s %8d %8d\n", n, text[n], bss[n]
    }
    printf "%-16s %8d %8d\n", "total", total, tbss
}'
fi

echo
echo "Schema is data, not code: the descriptor set a firmware carries costs flash, for example"
for f in bindings/c/tests/data/matrix3.desc bindings/c/tests/data/maptest.desc tests/data/edge.desc; do
    if [ -f "$ROOT/$f" ]; then
        printf "  %-40s %5s bytes\n" "$f" "$(wc -c <"$ROOT/$f" | tr -d ' ')"
    fi
done
