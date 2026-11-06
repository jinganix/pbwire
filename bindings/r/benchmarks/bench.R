# R binding benchmark on matrix3.proto: the four standard tiers from
# BENCHMARKS.md (small / medium / large / xlarge scale the repeated fields
# x0 / x1 / x100 / x3000). Against RProtoBuf when it is installed and loads
# the same schema; otherwise the baseline columns print "-".
#
# Both sides must produce identical bytes before anything is timed. Like the
# other harnesses it calibrates from the floor until one pass takes
# 0.25 s, warms up, then reports the best of 3 passes.
#
#   Rscript benchmarks/bench.R   (from bindings/r, after ./build.sh)

here <- dirname(sub("^--file=", "", grep("^--file=", commandArgs(),
                                         value = TRUE)[1L]))
dyn.load(file.path(here, "..", "pbwire.so"))
source(file.path(here, "..", "runtime", "R", "pbwire.R"))

schema <- pbwire_loadfile("../../c/tests/data/matrix3.desc")
MSG <- "structtest.Scalars3"

I32MIN <- -2147483647 - 1
I64MIN <- -9223372036854775807 - 1

fill <- function(repeats) {
  m <- list(
    i32 = I32MIN + 1L, i64 = 2^63, # 2^63 clamps to INT64_MAX on encode, reads back as 2^63
    u32 = 4294967295, u64 = -1, # bit pattern for 2^64-1, like the Lua bench
    s32 = I32MIN + 1L, s64 = I64MIN,
    fx32 = 3735928559, fx64 = 72623859790382856,
    sfx32 = I32MIN + 1L, sfx64 = I64MIN,
    f32 = Inf, f64 = -123456.789, flag = TRUE,
    str = "hello", color = 2L, inner = list(name = "kid", value = 42L),
    opt_i32 = -7L, opt_str = "opt",
    nums = integer(0), tags = character(0), inners = list(), flags = logical(0)
  )
  if (repeats > 0) {
    m$nums <- seq_len(repeats) - 4L
    m$tags <- rep("tag", repeats)
    m$inners <- lapply(seq_len(repeats),
                       function(i) list(name = "grandkid", value = i))
    m$flags <- rep(c(TRUE, FALSE), length.out = repeats)
  }
  m
}

# Optional baseline: RProtoBuf on the same matrix3.proto (best effort: any
# load failure just disables the baseline columns). R has no uint64, so the
# u64 max the payload carries as -1 is set from its decimal string, which
# serializes to the same 10-byte varint; every other field is the same R
# value pbwire encodes (verified identical on all four tiers before timing).
# Decode is compared like for like: RProtoBuf::read parses lazily into a
# Message, while pbwire_decode returns a fully materialized named list, so
# the baseline side reads every field into the same plain-list shape (a bare
# read() is faster but hands back no R data; see BENCHMARKS.md). `[[` avoids
# the `str` field colliding with the Message method of the same name, and
# absent repeated fields materialize as empty lists, exactly like pbwire.
has_pb <- FALSE
pb_encode <- NULL
pb_decode <- NULL
tryCatch({
  if (requireNamespace("RProtoBuf", quietly = TRUE)) {
    RProtoBuf::readProtoFiles("../../c/tests/data/matrix3.proto")
    scalars3 <- RProtoBuf::P("structtest.Scalars3")
    inner3 <- RProtoBuf::P("structtest.Inner3")
    pb_encode <- function(tbl) {
      m <- RProtoBuf::new(scalars3)
      m$i32 <- tbl$i32; m$i64 <- tbl$i64; m$u32 <- tbl$u32
      m$u64 <- "18446744073709551615"
      m$s32 <- tbl$s32; m$s64 <- tbl$s64; m$fx32 <- tbl$fx32; m$fx64 <- tbl$fx64
      m$sfx32 <- tbl$sfx32; m$sfx64 <- tbl$sfx64; m$f32 <- tbl$f32
      m$f64 <- tbl$f64; m$flag <- tbl$flag; m$str <- tbl$str
      m$color <- tbl$color
      m$inner <- RProtoBuf::new(inner3, name = tbl$inner$name,
                                value = tbl$inner$value)
      m$opt_i32 <- tbl$opt_i32; m$opt_str <- tbl$opt_str
      m$nums <- tbl$nums; m$tags <- tbl$tags
      m$inners <- lapply(tbl$inners,
                          function(x) RProtoBuf::new(inner3, name = x$name,
                                                    value = x$value))
      m$flags <- tbl$flags
      RProtoBuf::serialize(m, NULL)
    }
    pb_mat_inner <- function(x) list(name = x[["name"]], value = x[["value"]])
    pb_present <- function(x) if (length(x) == 0) list() else x
    pb_materialize <- function(m) {
      list(i32 = m[["i32"]], i64 = m[["i64"]], u32 = m[["u32"]],
        u64 = m[["u64"]], s32 = m[["s32"]], s64 = m[["s64"]],
        fx32 = m[["fx32"]], fx64 = m[["fx64"]],
        sfx32 = m[["sfx32"]], sfx64 = m[["sfx64"]],
        f32 = m[["f32"]], f64 = m[["f64"]], flag = m[["flag"]],
        str = m[["str"]], color = m[["color"]],
        inner = pb_mat_inner(m[["inner"]]),
        opt_i32 = m[["opt_i32"]], opt_str = m[["opt_str"]],
        nums = pb_present(m[["nums"]]), tags = pb_present(m[["tags"]]),
        inners = pb_present(lapply(m[["inners"]], pb_mat_inner)),
        flags = pb_present(m[["flags"]]))
    }
    pb_decode <- function(wire) pb_materialize(RProtoBuf::read(scalars3, wire))
    has_pb <- TRUE
  }
}, error = function(e) has_pb <<- FALSE)

bench_encode <- function(tbl) pbwire_encode(schema, MSG, tbl)
bench_decode <- function(wire) pbwire_decode(schema, MSG, wire)

calibrate <- function(op, floor) {
  count <- max(floor, 1)
  repeat {
    t0 <- proc.time()[["elapsed"]]
    for (i in seq_len(count)) op()
    if (proc.time()[["elapsed"]] - t0 >= 0.25 || count >= 1e8) break
    count <- count * 2
  }
  count
}

best <- function(op, count) {
  op() # warm up
  b <- Inf
  for (r in 1:3) {
    t0 <- proc.time()[["elapsed"]]
    for (i in seq_len(count)) op()
    dt <- (proc.time()[["elapsed"]] - t0) / count
    if (dt < b) b <- dt
  }
  b
}

cases <- list(
  list("small", 0, 136, 200000),
  list("medium", 1, 174, 200000),
  list("large", 100, 2469, 20000),
  list("xlarge", 3000, 74913, 2000)
)

for (c in cases) {
  name <- c[[1]]; repeats <- c[[2]]; want <- c[[3]]; floor <- c[[4]]
  tbl <- fill(repeats)
  wire <- bench_encode(tbl)
  # Gate: tier size (our bytes are canonical), spot-checked fields, and a
  # full decode -> encode round trip. R numerics cannot spell fx64 exactly
  # past 2^53, so it is covered by the round trip, not by value equality.
  stopifnot(length(wire) == want)
  back <- bench_decode(wire)
  stopifnot(back$i32 == -2147483647, back$i64 == 2^63)
  stopifnot(back$u32 == 4294967295, back$flag == TRUE)
  stopifnot(back$f32 == Inf, back$f64 == -123456.789)
  stopifnot(back$str == "hello", back$color == 2L)
  stopifnot(back$inner$name == "kid", back$inner$value == 42L)
  stopifnot(back$opt_i32 == -7L, back$opt_str == "opt")
  stopifnot(length(back$nums) == repeats, length(back$tags) == repeats,
            length(back$inners) == repeats, length(back$flags) == repeats)
  if (repeats > 0) {
    stopifnot(back$tags[1] == "tag")
    stopifnot(back$inners[[repeats]]$value == repeats)
  }
  stopifnot(identical(bench_encode(back), wire))
  # Gate the baseline too: same bytes before anything is timed.
  # Gate the baseline too: same bytes before anything is timed, and the
  # same materialized list (u64 has no exact R spelling on either side).
  if (has_pb) {
    stopifnot(identical(pb_encode(tbl), wire))
    ref <- bench_decode(wire); got <- pb_decode(wire)
    ref$u64 <- NULL; got$u64 <- NULL
    stopifnot(identical(got[names(ref)], ref))
  }

  ec <- calibrate(function() bench_encode(tbl), floor)
  dc <- calibrate(function() bench_decode(wire), floor)
  # Builder hot path: built once outside timing, so encode() is one core
  # walk plus one buffer malloc.
  built <- pbwire_message(schema, MSG)
  pbwire_msg_set(built, tbl)
  stopifnot(identical(pbwire_msg_encode(built), wire))
  mc <- calibrate(function() pbwire_msg_encode(built), floor)
  ens <- best(function() bench_encode(tbl), ec) * 1e9
  dns <- best(function() bench_decode(wire), dc) * 1e9
  mns <- best(function() pbwire_msg_encode(built), mc) * 1e9
  embps <- length(wire) / ens * 1000
  dmbps <- length(wire) / dns * 1000
  mmbps <- length(wire) / mns * 1000
  cat(sprintf("%s: %d bytes, %d iterations (repeat field x%d)\n",
              name, length(wire), ec, repeats))
  cat("  impl      op           ns/op       MB/s\n")
  cat(sprintf("  pbwire    encode    %10.1f %10.1f\n", ens, embps))
  cat(sprintf("  pbwire    encode_msg%10.1f %10.1f\n", mns, mmbps))
  cat(sprintf("  pbwire    decode    %10.1f %10.1f\n", dns, dmbps))
  if (has_pb) {
    be <- best(function() pb_encode(tbl), ec) * 1e9
    bd <- best(function() pb_decode(wire), dc) * 1e9
    cat(sprintf("  base      encode    %10.1f %10.1f\n", be,
                length(wire) / be * 1000))
    cat(sprintf("  base      decode    %10.1f %10.1f\n", bd,
                length(wire) / bd * 1000))
    cat(sprintf("  -- vs baseline: encode %.2fx, encode_msg %.2fx, decode %.2fx\n",
                be / ens, be / mns, bd / dns))
  }
}
