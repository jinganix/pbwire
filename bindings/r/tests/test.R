# Self-check for the R binding: named lists <-> wire bytes over the C core.
# One ok/FAIL line per check, non-zero exit on failure. The descriptor sets
# are checked in (lua/tests/data, c/tests/data), so no protoc is needed.
#
#   Rscript tests/test.R   (from bindings/r, after ./build.sh)

here <- dirname(sub("^--file=", "", grep("^--file=", commandArgs(),
                                         value = TRUE)[1L]))
dyn.load(file.path(here, "..", "pbwire.so"))
source(file.path(here, "..", "runtime", "R", "pbwire.R"))

failures <- 0
ok <- function(cond, name) {
  if (isTRUE(cond)) {
    cat("ok", name, "\n")
  } else {
    failures <<- failures + 1
    cat("FAIL", name, "\n")
  }
}
fails <- function(f) {
  tryCatch({ f(); FALSE }, error = function(e) TRUE)
}

schema <- pbwire_loadfile("../lua/tests/data/sample.desc")
ok(!is.null(schema), "loadfile returns a schema")

msg <- "pbwire.lua.Sample"
full <- list(
  i32 = -7L, i64 = -1234567890123, u32 = 4000000000, u64 = 12345678901234,
  flag = TRUE, f32 = 1.5, f64 = -123456.789, name = "alice",
  blob = as.raw(c(0x62, 0x69, 0x6e, 0x00, 0x61, 0x72, 0x79)), color = 1L,
  inner = list(s = "nested"),
  nums = c(1L, -5L, 300L), tags = c("a", "bb"),
  inners = list(list(s = "x"), list(s = "y")),
  attrs = list(list(key = "a", value = 1L)), pick_num = 9L
)

# Round trip through the wire.
wire <- pbwire_encode(schema, msg, full)
ok(is.raw(wire) && length(wire) > 0, "encode returns bytes")
back <- pbwire_decode(schema, msg, wire)
ok(isTRUE(back$i32 == -7L), "int32 round trip")
ok(isTRUE(back$i64 == -1234567890123), "int64 round trip")
ok(isTRUE(back$u32 == 4000000000), "uint32 round trip")
ok(isTRUE(back$u64 == 12345678901234), "uint64 round trip")
ok(isTRUE(back$flag == TRUE), "bool round trip")
ok(isTRUE(back$f32 == 1.5), "float round trip")
ok(isTRUE(back$f64 == -123456.789), "double round trip")
ok(isTRUE(back$name == "alice"), "string round trip")
ok(identical(back$blob, as.raw(c(0x62, 0x69, 0x6e, 0x00, 0x61, 0x72, 0x79))),
   "bytes round trip (embedded NUL)")
ok(isTRUE(back$color == 1L), "enum round trip")
ok(isTRUE(back$inner$s == "nested"), "nested message round trip")
ok(length(back$nums) == 3 && back$nums[1] == 1L && back$nums[3] == 300L,
   "packed repeated round trip")
ok(length(back$tags) == 2 && back$tags[2] == "bb",
   "repeated string round trip")
ok(length(back$inners) == 2 && back$inners[[2]]$s == "y",
   "repeated message round trip")
ok(length(back$attrs) == 1 && back$attrs[[1]]$key == "a" &&
   back$attrs[[1]]$value == 1L, "map as entry list round trip")
ok(isTRUE(back$pick_num == 9L) && is.null(back$pick_str),
   "oneof round trip")

# Re-encoding a decoded list gives the same bytes.
ok(identical(pbwire_encode(schema, msg, back), wire),
   "decode/encode is byte-exact")

# Absent fields decode as NULL / zero-length vectors.
sparse <- pbwire_decode(schema, msg, pbwire_encode(schema, msg,
                                                   list(name = "bob")))
ok(isTRUE(sparse$name == "bob"), "sparse field present")
ok(is.null(sparse$i32), "absent singular is NULL")
ok(is.null(sparse$flag), "absent bool is NULL (not FALSE)")
ok(is.null(sparse[["inner"]]), "absent message is NULL")  # [[ ]]: $ would partial-match 'inners'
ok(length(sparse$nums) == 0, "absent repeated is empty")

# false and 0 are present and survive.
present <- pbwire_decode(schema, msg,
                         pbwire_encode(schema, msg,
                                       list(flag = FALSE, i32 = 0L)))
ok(isTRUE(present$flag == FALSE), "false is present")
ok(isTRUE(present$i32 == 0L), "zero is present")

# Reference payload decodes intact.
ref_back <- pbwire_decode(schema, msg,
                          pbwire_encode(schema, msg,
                                        list(i32 = -7L, name = "alice")))
ok(isTRUE(ref_back$i32 == -7L) && isTRUE(ref_back$name == "alice"),
   "reference payload decodes")

# Error paths: every bad call raises an R error, never a segfault.
ok(fails(function() pbwire_encode(schema, "pbwire.lua.Nope", list())),
   "unknown message type fails")
ok(fails(function() pbwire_encode(schema, msg, list(nope = 1L))),
   "unknown field fails")
ok(fails(function() pbwire_encode(schema, msg, list(i32 = "seven"))),
   "wrong value type fails")
one <- pbwire_decode(schema, msg, pbwire_encode(schema, msg, list(nums = 7L)))
ok(length(one$nums) == 1 && one$nums[[1]] == 7L, "length-1 vector is one element (R has no scalars)")
ok(fails(function() pbwire_encode(schema, msg, list(inner = "flat"))),
   "scalar for message field fails")
ok(fails(function() pbwire_decode(schema, msg, as.raw(c(0xff, 0xff, 0xff)))),
   "malformed wire fails")
ok(fails(function() pbwire_decode(schema, msg, as.raw(c(0x08, 0x96)))),
   "truncated varint fails")
ok(fails(function() pbwire_encode(schema, msg,
                                 structure(list(1L), names = ""))),
   "empty field name fails")
ok(fails(function() pbwire_load_raw(as.raw(c(1, 2, 3)))), "bad descriptor set fails")
ok(fails(function() pbwire_loadfile("tests/data/missing.desc")),
   "missing descriptor file fails")

# Insertion order never leaks into the bytes (the core emits descriptor
# order): equal lists encode equally, like protoc.
t1 <- list(name = "alice", i32 = -7L, nums = c(1L, 2L, 3L),
           inner = list(s = "x"))
t2 <- list(inner = list(s = "x"), nums = c(1L, 2L, 3L), i32 = -7L,
           name = "alice")
ok(identical(pbwire_encode(schema, msg, t1), pbwire_encode(schema, msg, t2)),
   "order-independent bytes")

# Explicit presence: false/0/"" are written out (the reference encoder
# would omit a proto3 zero, both parsers read it back the same).
zeros <- pbwire_encode(schema, msg, list(i32 = 0L, flag = FALSE, name = ""))
ok(identical(zeros, as.raw(c(0x08, 0x00, 0x28, 0x00, 0x42, 0x00))),
   "explicit zeros are present")
zback <- pbwire_decode(schema, msg, zeros)
ok(isTRUE(zback$i32 == 0L) && isTRUE(zback$flag == FALSE) &&
   isTRUE(zback$name == ""), "explicit zeros round trip")

# load() from bytes, empty message, empty input.
desc_con <- file("../lua/tests/data/sample.desc", "rb")
desc_bytes <- readBin(desc_con, "raw", n = file.info(
  "../lua/tests/data/sample.desc")$size)
close(desc_con)
schema2 <- pbwire_load(desc_bytes)
ok(identical(pbwire_encode(schema2, msg, list(name = "x")),
             pbwire_encode(schema, msg, list(name = "x"))),
   "load from bytes")
ok(length(pbwire_encode(schema, msg, list())) == 0,
   "empty list encodes empty")
eback <- pbwire_decode(schema, msg, raw(0))
ok(is.null(eback[["inner"]]) && length(eback$nums) == 0,  # [[ ]]: $ would partial-match 'inners'
   "empty input decodes sparse")

# Strict value types: no silent coercions.
ok(fails(function() pbwire_encode(schema, msg, list(flag = 1L))),
   "number for bool fails")
ok(fails(function() pbwire_encode(schema, msg, list(i32 = "7"))),
   "numeric string fails")
ok(fails(function() pbwire_encode(schema, msg, list(color = "COLOR_NOPE"))),
   "unknown enum name fails")
ok(isTRUE(pbwire_decode(schema, msg,
                        pbwire_encode(schema, msg,
                                      list(color = "COLOR_RED")))$color == 1L),
   "enum name encodes")

# Depth cap: 300 nested lists fail, 10 work.
deep <- list(v = 0L)
for (i in 1:300) deep <- list(`next` = deep, v = 1L)
ok(fails(function() pbwire_encode(schema, "pbwire.lua.Chain", deep)),
   "300-deep encode fails")
shallow <- list(v = 0L)
for (i in 1:10) shallow <- list(`next` = shallow, v = 1L)
sback <- pbwire_decode(schema, "pbwire.lua.Chain",
                       pbwire_encode(schema, "pbwire.lua.Chain", shallow))
ok(isTRUE(sback$`next`$`next`$v == 1L), "10-deep round trip")

# proto2: required, closed enums, groups, defaults.
s2 <- pbwire_loadfile("../lua/tests/data/sample2.desc")
m2 <- "pbwire.lua2.Sample2"
ok(fails(function() pbwire_decode(s2, m2, pbwire_encode(s2, m2, list()))),
   "missing required fails on decode")
wreq <- pbwire_encode(s2, m2, list(need = 5L))
ok(isTRUE(pbwire_decode(s2, m2, wreq)$need == 5L), "required round trip")
ok(is.null(pbwire_decode(s2, m2, wreq)$opt), "proto2 default not filled")
wu <- as.raw(c(0x08, 0x01, 0x18, 99))
ok(is.null(pbwire_decode(s2, m2, wu)$color), "unknown closed enum drops")
wue <- pbwire_encode(s2, m2, list(need = 1L, color = 99L))
ok(is.null(pbwire_decode(s2, m2, wue)$color), "unknown closed enum re-drops")
# Groups encode like messages (START_GROUP framing) and decode to lists.
wge <- pbwire_encode(s2, m2, list(need = 1L, mygroup = list(gv = 7L)))
wg <- as.raw(c(0x08, 0x01, 0x23, 0x28, 0x07, 0x24))
ok(identical(wge, wg), "group encode is START_GROUP framing")
gback <- pbwire_decode(s2, m2, wg)
ok(isTRUE(gback$need == 1L), "group bytes keep the rest intact")
ok(isTRUE(gback$mygroup$gv == 7L), "group decodes")
ok(identical(pbwire_encode(s2, m2, gback), wg),
   "group decode/encode byte-exact")

# Reference bytes: protoc --encode agrees with the binding.
gschema <- pbwire_loadfile("../c/tests/data/grouptest.desc")
gmsg <- "structtest.GroupTest"
gfull <- list(inner = list(x = 5L, s = "g"),
              item = list(list(v = 1L), list(v = -2L)), id = 9L)
gwire <- pbwire_encode(gschema, gmsg, gfull)
gref <- file("../c/tests/data/grouptest.bin", "rb")
gexpect <- readBin(gref, "raw", n = 1e6)
close(gref)
ok(identical(gwire, gexpect), "group encode equals protoc reference bytes")
gback2 <- pbwire_decode(gschema, gmsg, gexpect)
ok(isTRUE(gback2$inner$x == 5L && gback2$inner$s == "g"),
   "reference singular group decodes")
ok(length(gback2$item) == 2 && isTRUE(gback2$item[[2]]$v == -2L),
   "reference repeated group decodes")

# Builder: build once, encode hot. Matches the list API byte for byte.
ok(fails(function() pbwire_message(schema, "pbwire.lua.Nope")),
   "builder unknown type fails")
b <- pbwire_message(schema, msg)
pbwire_msg_set(b, list(i32 = -7L, name = "alice", nums = c(1L, 2L, 3L)))
ok(identical(pbwire_msg_encode(b),
             pbwire_encode(schema, msg,
                           list(i32 = -7L, name = "alice",
                                nums = c(1L, 2L, 3L)))),
   "builder matches list api")
pbwire_msg_set(b, list(name = "bob"))
bb <- pbwire_decode(schema, msg, pbwire_msg_encode(b))
ok(isTRUE(bb$name == "bob") && isTRUE(bb$i32 == -7L) &&
   length(bb$nums) == 3, "builder set merges")
pbwire_msg_set(b, list(nums = 9L))
bnb <- pbwire_decode(schema, msg, pbwire_msg_encode(b))
ok(length(bnb$nums) == 1 && bnb$nums[1] == 9L,
   "builder repeated set replaces")
# The builder anchors its schema: collecting the schema must not break it.
anchored <- local({
  s3 <- pbwire_loadfile("../lua/tests/data/sample.desc")
  m3 <- pbwire_message(s3, msg)
  pbwire_msg_set(m3, list(i32 = 42L))
  m3
})
rm(s3)
gc()
ok(isTRUE(pbwire_decode(schema, msg, pbwire_msg_encode(anchored))$i32 == 42L),
   "builder outlives its schema")

# Wire-level crafts: unpacked form of a packed field, oneof last-wins on
# the wire, int map key 0, full-byte blob, unicode, 64-bit extremes.
unpacked <- pbwire_decode(schema, msg, as.raw(c(0x60, 0x01, 0x60, 0x02)))
ok(length(unpacked$nums) == 2 && unpacked$nums[1] == 1L &&
   unpacked$nums[2] == 2L, "unpacked varints decode as packed")
oneof_a <- pbwire_decode(schema, msg,
                         as.raw(c(0x82, 0x01, 0x01, 0x78, 0x88, 0x01, 0x09)))
ok(isTRUE(oneof_a$pick_num == 9L) && is.null(oneof_a$pick_str),
   "oneof wire last wins (num)")
oneof_b <- pbwire_decode(schema, msg,
                         as.raw(c(0x88, 0x01, 0x09, 0x82, 0x01, 0x01, 0x78)))
ok(isTRUE(oneof_b$pick_str == "x") && is.null(oneof_b$pick_num),
   "oneof wire last wins (str)")
stick0 <- pbwire_decode(schema, msg,
                        as.raw(c(0x92, 0x01, 0x05, 0x08, 0x00, 0x12, 0x01,
                                 0x7a)))
ok(length(stick0$sticks) == 1 && stick0$sticks[[1]]$key == 0L &&
   stick0$sticks[[1]]$value == "z", "int map key 0 round trip")
allbytes <- as.raw(0:255)
ok(identical(pbwire_decode(schema, msg,
                           pbwire_encode(schema, msg, list(blob = allbytes)))$blob,
             allbytes), "256-byte blob round trip")
ok(isTRUE(pbwire_decode(schema, msg,
                        pbwire_encode(schema, msg,
                                      list(name = "h\u00e9llo")))$name ==
          "h\u00e9llo"), "unicode round trip")
i64min <- -9223372036854775807 - 1
extremes <- pbwire_decode(schema, msg,
                          pbwire_encode(schema, msg,
                                        list(i64 = i64min, u64 = 2^64)))
ok(identical(extremes$i64, -(2^53 * 1024)), "int64 min clamps exactly")  # R literals cannot spell INT64_MIN; -(2^53*1024) is exactly -2^63
ok(isTRUE(extremes$u64 == 2^64), "uint64 max is nearest double")
fz <- pbwire_encode(schema, msg, list(f32 = 0.1))
ok(identical(pbwire_encode(schema, msg, pbwire_decode(schema, msg, fz)), fz),
   "float32 idempotent")

# roundtrip: gateway passthrough. Unknown fields survive byte for byte;
# canonical bytes pass through unchanged.
with_unknown <- c(wire, as.raw(c(0x98, 0x06, 0x07))) # field 99 varint 7
rt <- pbwire_roundtrip(schema, msg, with_unknown)
ok(identical(rt, with_unknown), "roundtrip preserves unknown fields")
ok(isTRUE(pbwire_decode(schema, msg, rt)$name == back$name),
   "roundtrip keeps values")
ok(identical(pbwire_roundtrip(schema, msg, wire), wire),
   "roundtrip canonical stable")
ok(fails(function() pbwire_roundtrip(schema, msg, as.raw(c(0xff, 0xff, 0xff)))),
   "roundtrip malformed fails")
ok(fails(function() pbwire_roundtrip(schema, "pbwire.lua.Nope", wire)),
   "roundtrip unknown type fails")

# JSON/text mapping through the binding (lists and builders agree).
jsample <- list(i32 = -7L, name = "alice")
jstr <- pbwire_to_json(schema, msg, jsample)
ok(identical(jstr, '{"i32":-7,"name":"alice"}'), "to_json prints")
jback <- pbwire_from_json(schema, msg, jstr)
ok(isTRUE(jback$i32 == -7L) && identical(jback$name, "alice"),
   "from_json round trip")
ok(identical(pbwire_encode(schema, msg, jback),
             pbwire_encode(schema, msg, jsample)),
   "JSON round trip is wire-equal")
ok(fails(function() pbwire_from_json(schema, msg, "{oops")),
   "malformed JSON fails")
ok(fails(function() pbwire_from_json(schema, msg, '{"nope":1}')), "unknown JSON field fails")
ok(is.null(pbwire_from_json(schema, msg, '{"nope":1}', TRUE)$name),
   "ignored unknown JSON field reads as absent")
tstr <- pbwire_to_text(schema, msg, jsample)
ok(grepl('name: "alice"', tstr, fixed = TRUE), "to_text prints")
tback <- pbwire_from_text(schema, msg, tstr)
ok(identical(pbwire_encode(schema, msg, tback),
             pbwire_encode(schema, msg, jsample)),
   "text round trip is wire-equal")
ok(fails(function() pbwire_from_text(schema, msg, "name: ")),
   "malformed text fails")
# Builders parse exactly (64-bit integers past 2^53 survive, unlike lists).
big <- pbwire_parse_json(schema, msg, '{"u64":18446744073709551615}')
ok(grepl("18446744073709551615", pbwire_msg_to_json(big), fixed = TRUE),
   "builder JSON keeps uint64 max")
bigbin <- pbwire_parse_binary(schema, msg,
                              pbwire_encode(schema, msg, list(u64 = -1)))
ok(grepl("18446744073709551615", pbwire_msg_to_json(bigbin), fixed = TRUE),
   "builder binary keeps uint64 max")
# Builder to_text drops unknown fields on request (conformance flag).
wparsed <- pbwire_parse_binary(schema, msg,
                              c(pbwire_encode(schema, msg, jsample),
                                as.raw(c(0x98, 0x06, 0x07))))
ok(grepl("99", pbwire_msg_to_text(wparsed), fixed = TRUE),
   "builder to_text prints unknowns by default")
ok(!grepl("99", pbwire_msg_to_text(wparsed, FALSE), fixed = TRUE),
   "builder to_text(FALSE) drops unknowns")
# Deterministic encoding sorts map entries by key.
mschema <- pbwire_loadfile("../c/tests/data/maptest.desc")
mmsg <- "structtest.Maps"
mentry <- function(k, v) as.raw(c(0x0a, 0x05, 0x08, k, 0x12, 0x01, v))
unordered <- c(mentry(2L, 0x62), mentry(1L, 0x61))
expected <- c(mentry(1L, 0x61), mentry(2L, 0x62))
det <- pbwire_encode_deterministic(mschema, mmsg,
                                   pbwire_decode(mschema, mmsg, unordered))
ok(identical(det, expected), "deterministic encoding sorts map entries")
# Delimited frames concatenate and decode in a loop.
f1 <- pbwire_encode_delimited(schema, msg, jsample)
f2 <- pbwire_encode_delimited(schema, msg, list(name = "bob"))
frames <- c(f1, f2)
at <- 1L
names <- c()
while (at <= length(frames)) {
  fr <- pbwire_decode_delimited(schema, msg, frames[at:length(frames)])
  ok(fr$read > 0L, "frame reports its size")
  names <- c(names, fr$message$name)
  at <- at + fr$read
}
ok(identical(names, c("alice", "bob")), "delimited frames decode in order")
ok(fails(function() pbwire_decode_delimited(schema, msg, frames[1:2])),
   "truncated frame fails")
# Unknown fields survive byte for byte through roundtrip (gateway
# passthrough); lists carry known fields only, like the JSON mapping.
ok(identical(pbwire_roundtrip(schema, msg, with_unknown), with_unknown),
   "unknown fields survive byte round trip")

cat(if (failures == 0) "all tests passed\n" else
    paste0(failures, " failures\n"))
quit(save = "no", status = if (failures == 0) 0 else 1)
