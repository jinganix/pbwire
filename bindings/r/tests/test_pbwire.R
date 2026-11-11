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

## visit: single-level scan with no tree (field 8 "name" is LEN "alice").
events <- pbwire_visit(schema, msg, wire)
by_num <- list()
for (ev in events) by_num[[as.character(ev$number)]] <- ev
ok(by_num[["8"]]$wire_type == 2L &&
   identical(by_num[["8"]]$value, charToRaw("alice")),
   "visit sees the LEN string")
ok(by_num[["1"]]$wire_type == 0L, "visit sees the varint wire type")
ok(fails(pbwire_visit(schema, msg, wire, list(max_bytes = 4))), "visit honors max_bytes")

## encoded_size matches; encode_to honors a fixed cap (size + 12 always fits).
size <- pbwire_encoded_size(schema, msg, full)
ok(size == length(wire), "encoded_size matches the wire length")
ok(identical(pbwire_encode_to(schema, msg, full, size + 12), wire), "encode_to sized cap works")
ok(fails(pbwire_encode_to(schema, msg, full, 4)), "encode_to rejects a short cap")
ffull <- pbwire_encode_delimited(schema, msg, full)
ok(identical(pbwire_encode_delimited_to(schema, msg, full, length(ffull) + 12), ffull),
   "encode_delimited_to matches the frame")

## encode_sink delivers the encode in chunks; FALSE aborts.
pieces <- list()
pbwire_encode_sink(schema, msg, full, function(chunk) { pieces[[length(pieces) + 1L]] <<- chunk; TRUE })
ok(identical(do.call(c, pieces), wire), "sink gets every byte")
ok(fails(pbwire_encode_sink(schema, msg, full, function(chunk) FALSE)), "sink abort raises")

## Limits reject oversized input on every entry point.
ok(fails(pbwire_decode(schema, msg, wire, list(max_bytes = 4))), "decode honors max_bytes")
ok(fails(pbwire_parse_binary(schema, msg, wire, list(max_bytes = 4))), "parse_binary honors max_bytes")
ok(fails(pbwire_from_json(schema, msg, '{"name":"x"}', FALSE, list(max_bytes = 4))), "from_json honors max_bytes")
ok(fails(pbwire_from_text(schema, msg, 'name: "x"', list(max_bytes = 4))), "from_text honors max_bytes")
fr <- pbwire_encode_delimited(schema, msg, full)
ok(fails(pbwire_decode_delimited(schema, msg, fr, list(max_bytes = 2))), "decode_delimited honors max_bytes")
ok(fails(pbwire_load_raw(readBin("tests/data/sample.desc", "raw", n = 999999), list(max_bytes = 4))), "load honors max_bytes")

## Services, WKT helpers, and pooled contexts through the shared fixture.
aschema <- pbwire_load("../../tests/data/adv.desc")
ok(identical(pbwire_services(aschema), "pbwire.adv.Jobs"), "services lists Jobs")
jobs <- pbwire_service(aschema, "pbwire.adv.Jobs")
ok(jobs$name == "pbwire.adv.Jobs" && length(jobs$methods) == 2L, "service has two methods")
ok(jobs$methods[[1L]]$name == "Get" && jobs$methods[[1L]]$input_type == "pbwire.adv.Job", "unary resolves")
ok(isFALSE(jobs$methods[[1L]]$client_streaming) && isFALSE(jobs$methods[[1L]]$server_streaming),
   "unary flags decode")
ok(isTRUE(jobs$methods[[2L]]$client_streaming) && isTRUE(jobs$methods[[2L]]$server_streaming),
   "streaming flags decode")
routed <- pbwire_resolve(aschema, "pbwire.adv.Jobs", "Get")
ok(routed$input == "pbwire.adv.Job" && routed$output == "pbwire.adv.Job", "resolve maps names")
ok(fails(pbwire_service(aschema, "pbwire.adv.Nope")), "unknown service fails")
ok(fails(pbwire_resolve(aschema, "pbwire.adv.Jobs", "Nope")), "unknown method fails")

packed <- pbwire_any_pack(aschema, "google.protobuf.Any",
                          "type.googleapis.com/pbwire.adv.Job",
                          pbwire_encode(aschema, "pbwire.adv.Job", list(id = "7")))
ok(pbwire_msg_any_type_url(packed) == "type.googleapis.com/pbwire.adv.Job", "any keeps the type url")
ok(pbwire_decode(aschema, "pbwire.adv.Job", pbwire_msg_any_payload(packed))$id == "7", "any keeps the payload")
ts <- pbwire_timestamp_from_millis(aschema, "google.protobuf.Timestamp", 1501)
ok(pbwire_msg_timestamp_millis(ts) == 1501, "timestamp millis round trip")
du <- pbwire_duration_from_millis(aschema, "google.protobuf.Duration", -2500)
ok(pbwire_msg_duration_millis(du) == -2500, "duration millis round trip")
fm <- pbwire_parse_json(aschema, "google.protobuf.FieldMask", '{"paths":["a.b","c"]}')
ok(identical(pbwire_msg_fieldmask_paths(fm), c("a.b", "c")), "fieldmask paths read")
ok(fails(pbwire_any_pack(schema, msg, "t", charToRaw("p"))), "any_pack rejects a non-Any type")

ctx <- pbwire_new_context()
ok(identical(pbwire_context_encode(ctx, schema, msg, full), wire), "context encode matches")
cm <- pbwire_context_decode(ctx, schema, msg, wire)
ok(identical(pbwire_message_encode(cm), wire), "context decode reads back")
ok(fails(pbwire_message_set(cm, list())), "context tree is read-only")
pbwire_context_reset(ctx)
ok(fails(pbwire_message_encode(cm)), "reset invalidates the borrow")
ok(pbwire_context_used(ctx) >= 0, "used reports bytes held")
invisible(pbwire_context_retain_cap(ctx, 1024))
ok(pbwire_context_retain_cap(ctx) == 1024, "retain cap reads back")
pool <- pbwire_new_pool(2, 0)
pc <- pbwire_pool_acquire(pool)
ok(pbwire_pool_idle(pool) == 0L, "acquire pops the freelist")
invisible(pbwire_context_encode(pc, schema, msg, full))
pbwire_pool_release(pool, pc)
ok(pbwire_pool_idle(pool) == 1L, "release parks the context")
ok(fails(invisible(pbwire_context_encode(pc, schema, msg, full))), "released context raises")

if (failures > 0L) {
  cat(failures, "failures\n")
  quit(status = 1)
}
cat("all R binding checks passed\n")
