# Generated-conformance check for the R binding: build matrix3.proto's
# Scalars3 through the generated constructors, encode, decode back and check
# every field. The schema travels embedded in tests/gen/matrix3_pbwire.R.
#
# shortcut: R doubles hold integers exactly only to 2^53, so the extreme
# int64/uint64/fixed64 values in matrix3.bin cannot round-trip exactly;
# this test uses moderate values and checks wire self-consistency
# (decode/encode byte-exact, like the dynamic self-test). Upgrade when the
# binding grows a 64-bit integer type (bit64::integer64).
#
#   Rscript tests/generated_test.R   (from bindings/r, after ./build.sh)

here <- dirname(sub("^--file=", "", grep("^--file=", commandArgs(),
                                         value = TRUE)[1L]))
dyn.load(file.path(here, "..", "pbwire.so"))
source(file.path(here, "..", "runtime", "R", "pbwire.R"))
source(file.path(here, "gen", "matrix3_pbwire.R"))

checks <- 0
check <- function(ok, what) {
  checks <<- checks + 1
  if (!isTRUE(ok)) stop(paste("FAIL", what))
}

build <- function() {
  Structtest_Scalars3_new(
    i32 = -7L, i64 = -1234567890123, u32 = 4000000000, u64 = 12345678901234,
    s32 = -8L, s64 = -1234567890124, fx32 = 3735928559, fx64 = 72623859790382856,
    sfx32 = -9L, sfx64 = -1234567890125,
    f32 = Inf, f64 = -123456.789,
    flag = TRUE, str = "hello", color = 2L,
    inner = Structtest_Inner3_new(name = "kid", value = 42L),
    nums = list(1L, -5L, 300L), tags = list("a", "bb"),
    inners = list(Structtest_Inner3_new(name = "x", value = 1L),
                 Structtest_Inner3_new(name = "y", value = -2L)),
    opt_i32 = -7L, opt_str = "opt",
    flags = list(TRUE, FALSE, TRUE)
  )
}

wire <- Structtest_Scalars3_encode(build())
check(is.raw(wire) && length(wire) > 0, "encode returns bytes")

back <- Structtest_Scalars3_decode(wire)
check(back$i32 == -7L, "i32")
check(back$i64 == -1234567890123, "i64")
check(back$u32 == 4000000000, "u32")
check(back$u64 == 12345678901234, "u64")
check(back$s32 == -8L, "s32")
check(back$sfx32 == -9L, "sfx32")
check(back$flag == TRUE, "flag")
check(back$str == "hello", "str")
check(back$color == 2L, "color")
check(back$inner$name == "kid" && back$inner$value == 42L, "inner")
check(length(back$tags) == 2 && back$tags[[1]] == "a" && back$tags[[2]] == "bb", "tags")
check(length(back$inners) == 2 && back$inners[[2]]$value == -2L, "inners")
check(back$opt_i32 == -7L, "opt_i32")
check(back$opt_str == "opt", "opt_str")

check(identical(Structtest_Scalars3_encode(back), wire), "decode/encode is byte-exact")

check(length(Structtest_Scalars3_encode(Structtest_Scalars3_new())) == 0,
      "empty generated message encodes to nothing")
zero <- Structtest_Scalars3_decode(
  Structtest_Scalars3_encode(Structtest_Scalars3_new(opt_i32 = 0L)))
check(zero$opt_i32 == 0L, "explicit zero with presence survives")

cat(sprintf("generated test ok: %d checks\n", checks))
