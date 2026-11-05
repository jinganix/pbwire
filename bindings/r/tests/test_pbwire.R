## Self-check for the R binding: named lists <-> wire bytes over the C core.
## One ok/FAIL line per check, non-zero exit on failure. The descriptor set
## is checked in, so no protoc is needed at test time.
##
##   Rscript tests/test_pbwire.R   (from bindings/r, after ./build.sh)

dyn.load("pbwire.so")
source("runtime/R/pbwire.R")

failures <- 0L
ok <- function(cond, name) {
  if (isTRUE(cond)) {
    cat("ok", name, "\n")
  } else {
    failures <<- failures + 1L
    cat("FAIL", name, "\n")
  }
}

schema <- pbwire_load("tests/data/sample.desc")
ok(!is.null(schema), "load returns a schema")

msg <- "pbwire.lua.Sample"
full <- list(
  i32 = -7L, i64 = -1234567890123, u32 = 4000000000, u64 = 12345678901234,
  flag = TRUE, f32 = 1.5, f64 = -123456.789, name = "alice",
  blob = c(charToRaw("bin"), as.raw(0x00), charToRaw("ary")), color = 1L, inner = list(s = "nested"),
  nums = list(1L, -5L, 300L), tags = list("a", "bb"),
  inners = list(list(s = "x"), list(s = "y")),
  attrs = list(list(key = "a", value = 1L)), pick_num = 9L
)

## Round trip through the wire.
wire <- pbwire_encode(schema, msg, full)
ok(is.raw(wire) && length(wire) > 0, "encode returns bytes")
back <- pbwire_decode(schema, msg, wire)
ok(back$i32 == -7L, "int32 round trip")
ok(back$i64 == -1234567890123, "int64 round trip")
ok(back$u32 == 4000000000, "uint32 round trip")
ok(back$u64 == 12345678901234, "uint64 round trip")
ok(isTRUE(back$flag), "bool round trip")
ok(back$f32 == 1.5, "float round trip")
ok(back$f64 == -123456.789, "double round trip")
ok(back$name == "alice", "string round trip")
ok(identical(back$blob, c(charToRaw("bin"), as.raw(0x00), charToRaw("ary"))), "bytes round trip (embedded NUL)")
ok(back$color == 1L, "enum round trip")
ok(back$inner$s == "nested", "nested message round trip")
ok(length(back$nums) == 3 && back$nums[[1]] == 1L && back$nums[[3]] == 300L,
   "packed repeated round trip")
ok(length(back$tags) == 2 && back$tags[[2]] == "bb", "repeated string round trip")
ok(length(back$inners) == 2 && back$inners[[2]]$s == "y",
   "repeated message round trip")
ok(length(back$attrs) == 1 && back$attrs[[1]]$key == "a" &&
   back$attrs[[1]]$value == 1L, "map as entry list round trip")
ok(back$pick_num == 9L && is.null(back$pick_str), "oneof round trip")

## Re-encoding a decoded list gives the same bytes.
ok(identical(pbwire_encode(schema, msg, back), wire), "decode/encode is byte-exact")

## Absent fields decode as NULL / empty lists.
sparse <- pbwire_decode(schema, msg, pbwire_encode(schema, msg, list(name = "bob")))
ok(sparse$name == "bob", "sparse field present")
ok(is.null(sparse$i32), "absent singular is NULL")
ok(is.null(sparse$flag), "absent bool is NULL (not FALSE)")
ok(is.null(sparse[["inner"]]), "absent message is NULL")  # [[ ]]: $ would partial-match 'inners'
ok(is.list(sparse$nums) && length(sparse$nums) == 0,
   "absent repeated is an empty list")

## FALSE and 0 are present and survive.
present <- pbwire_decode(schema, msg, pbwire_encode(schema, msg,
                         list(flag = FALSE, i32 = 0L)))
ok(isFALSE(present$flag), "FALSE is present")
ok(present$i32 == 0L, "zero is present")

## Atomic vectors are accepted for repeated scalars.
vec <- pbwire_decode(schema, msg, pbwire_encode(schema, msg,
                     list(nums = c(1L, 2L, 3L), tags = c("a", "b"))))
ok(length(vec$nums) == 3 && vec$nums[[3]] == 3L, "integer vector for repeated")
ok(length(vec$tags) == 2 && vec$tags[[1]] == "a", "string vector for repeated")

## Enum names resolve; NULL means absent.
m <- list(color = "COLOR_RED", name = "x")
m["name"] <- list(NULL)
byname <- pbwire_decode(schema, msg, pbwire_encode(schema, msg, m))
ok(byname$color == 1L, "enum name round trip")
ok(is.null(byname$name), "NULL encodes as absent")

fails <- function(expr) {
  ok <- tryCatch({ expr; FALSE }, error = function(e) TRUE)
  ok
}
ok(fails(pbwire_encode(schema, "pbwire.lua.Nope", list())), "unknown message type fails")
ok(fails(pbwire_encode(schema, msg, list(nope = 1L))), "unknown field fails")
ok(fails(pbwire_encode(schema, msg, list(i32 = "seven"))), "wrong value type fails")
one <- pbwire_decode(schema, msg, pbwire_encode(schema, msg, list(nums = 7L)))
ok(length(one$nums) == 1 && one$nums[[1]] == 7L, "length-1 vector is one element (R has no scalars)")
ok(fails(pbwire_encode(schema, msg, list(inner = "flat"))), "scalar for message field fails")
ok(fails(pbwire_decode(schema, msg, as.raw(c(0xff, 0xff, 0xff)))), "malformed wire fails")
ok(fails(pbwire_decode(schema, msg, as.raw(c(0x08, 0x96)))), "truncated varint fails")
ok(fails(pbwire_encode(schema, msg, list(i32 = NA_integer_))), "NA is rejected")
ok(fails(pbwire_load("tests/data/missing.desc")), "missing descriptor file fails")
ok(fails(pbwire_load_raw(charToRaw("not a descriptor set"))), "bad descriptor set fails")

## Insertion order never leaks into the bytes (the core emits descriptor
## order): equal lists encode equally, like protoc.
t1 <- list(name = "alice", i32 = -7L, nums = list(1L, 2L, 3L),
           inner = list(s = "x"))
t2 <- list(inner = list(s = "x"), nums = list(1L, 2L, 3L), i32 = -7L,
           name = "alice")
ok(identical(pbwire_encode(schema, msg, t1), pbwire_encode(schema, msg, t2)),
   "order-independent bytes")

## Gateway passthrough keeps unknown fields: decode and re-encode in the
## core, so a re-encode is byte-exact even with unknown input.
ok(identical(pbwire_roundtrip(schema, msg, wire), wire), "roundtrip is byte-exact")

## Builder: set once, encode per send.
built <- pbwire_message(schema, msg)
pbwire_message_set(built, list(i32 = -7L, name = "alice"))
ok(identical(pbwire_message_encode(built),
             pbwire_encode(schema, msg, list(i32 = -7L, name = "alice"))),
   "builder matches one-shot encode")
pbwire_message_set(built, list(nums = list(1L, 2L)))
ok(pbwire_decode(schema, msg, pbwire_message_encode(built))$nums[[2]] == 2L,
   "builder set merges")

if (failures > 0L) {
  cat(failures, "failures\n")
  quit(status = 1)
}
cat("all R binding checks passed\n")
