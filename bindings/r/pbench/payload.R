# The one payload every R pbench arm times, and the RProtoBuf list it has to
# match: structtest.Scalars3 from bindings/c/tests/data/matrix3.proto scaled
# over the four standard tiers.
#
# The native object is the generated model's shape (tests/gen/matrix3_pbwire.R):
# a named list with the proto field names, a nested list for `inner`, atomic
# vectors for the repeated scalars and a list for the repeated messages -- what
# a user of the generated facade already passes around. The oracle is RProtoBuf
# on the same .proto, materialized into the same plain-list shape: RProtoBuf's
# `read` parses lazily into a Message, and leaving that lazy read out is what
# keeps the two sides the same task (see BENCHMARKS.md, R section).
#
#   source(file.path(<pbench dir>, "payload.R"))

pbench_dir <- function() {
  a <- grep("^--file=", commandArgs(trailingOnly = FALSE), value = TRUE)
  if (length(a) == 0) {
    return(getwd())
  }
  dirname(sub("^--file=", "", a[[1]]))
}

pbench_root <- normalizePath(file.path(pbench_dir(), ".."), mustWork = TRUE)

if (!"pbwire" %in% names(getLoadedDLLs())) {
  dyn.load(file.path(pbench_root, "pbwire.so"))
}
source(file.path(pbench_root, "runtime", "R", "pbwire.R"))
# The generated models: the embedded descriptor set plus the named-list
# constructors and encode/decode of structtest.Scalars3.
source(file.path(pbench_root, "tests", "gen", "matrix3_meta.R"))
source(file.path(pbench_root, "tests", "gen", "matrix3_pbwire.R"))

PB_TIERS <- list(
  list(label = "small", repeats = 0, bytes = 136, iterations = 200000),
  list(label = "medium", repeats = 1, bytes = 174, iterations = 200000),
  list(label = "large", repeats = 100, bytes = 2469, iterations = 20000),
  list(label = "xlarge", repeats = 3000, bytes = 74913, iterations = 2000)
)

PB_MSG <- "structtest.Scalars3"

# The schema behind the generated models, and the message slot the shim arm
# resolves once (the generated model's `Type_encode(list)` shape, without the
# per-call type-name lookup). bench.R keeps loading the .desc fixture; the arms
# here ride the generated path, which is the one a user holds.
pbench_schema <- local({
  schema <- NULL
  function() {
    if (is.null(schema)) {
      schema <<- .pbwire_get_schema_matrix3()
    }
    schema
  }
})

pbench_message_number <- local({
  number <- NULL
  function() {
    if (is.null(number)) {
      number <<- pbwire_type_number(pbench_schema(), PB_MSG)
    }
    number
  }
})

# The payload for `repeats` scaled elements, in the generated model's shape.
# Every field is present, including the two proto3 optionals. Two R spellings to
# keep: `INT32_MIN` reads back as `NA` (it *is* `NA_INTEGER`), so the payload
# carries `INT32_MIN + 1` like benchmarks/bench.R; and `u64 = -1` is the
# two's-complement bit pattern of 2^64-1, since an R double cannot spell it.
pbench_payload <- function(repeats) {
  i32min <- -2147483647 - 1
  i64min <- -9223372036854775807 - 1
  out <- Structtest_Scalars3_new(
    i32 = i32min + 1L, i64 = 2^63, u32 = 4294967295, u64 = -1,
    s32 = i32min + 1L, s64 = i64min,
    fx32 = 3735928559, fx64 = 72623859790382856,
    sfx32 = i32min + 1L, sfx64 = i64min,
    f32 = Inf, f64 = -123456.789, flag = TRUE,
    str = "hello", color = 2L,
    inner = Structtest_Inner3_new(name = "kid", value = 42L),
    opt_i32 = -7L, opt_str = "opt",
    nums = integer(0), tags = character(0), inners = list(),
    flags = logical(0)
  )
  if (repeats > 0) {
    out$nums <- seq_len(repeats) - 4L
    out$tags <- rep("tag", repeats)
    out$inners <- lapply(seq_len(repeats),
                         function(i) Structtest_Inner3_new(name = "grandkid",
                                                           value = i))
    out$flags <- rep(c(TRUE, FALSE), length.out = repeats)
  }
  out
}

# Every field benchmarks/bench.R reads, so a decode is gated field by field,
# not only by its re-encoded bytes.
pbench_reads_ok <- function(back, repeats) {
  isTRUE(
    identical(back$i32, -2147483647L) &&
      identical(back$i64, 2^63) &&
      identical(back$u32, 4294967295) &&
      identical(back$s32, -2147483647L) &&
      identical(back$sfx32, -2147483647L) &&
      identical(back$f32, Inf) &&
      identical(back$f64, -123456.789) &&
      identical(back$flag, TRUE) &&
      identical(back$str, "hello") &&
      identical(back$color, 2L) &&
      identical(back$inner$name, "kid") &&
      identical(back$inner$value, 42L) &&
      identical(back$opt_i32, -7L) &&
      identical(back$opt_str, "opt") &&
      length(back$nums) == repeats &&
      length(back$tags) == repeats &&
      length(back$inners) == repeats &&
      length(back$flags) == repeats &&
      (repeats == 0 ||
         (identical(back$tags[[1]], "tag") &&
            identical(back$inners[[repeats]]$value, as.integer(repeats)) &&
            identical(back$nums[[repeats]], as.integer(repeats) - 4L)))
  )
}

# ---------------------------------------------------------------- baseline
#
# RProtoBuf on the same matrix3.proto: the bytes every arm must reproduce and
# the plain-list materialization a decode is compared against. Any load failure
# raises: the gate exists to compare against this.

pbench_baseline <- local({
  state <- NULL
  function() {
    if (!is.null(state)) {
      return(state)
    }
    if (!requireNamespace("RProtoBuf", quietly = TRUE)) {
      stop("pbwire: pbench needs RProtoBuf for the baseline")
    }
    RProtoBuf::readProtoFiles(file.path(pbench_root, "..", "c", "tests", "data",
                                        "matrix3.proto"))
    scalars <- RProtoBuf::P("structtest.Scalars3")
    inner <- RProtoBuf::P("structtest.Inner3")
    mat_inner <- function(x) list(name = x[["name"]], value = x[["value"]])
    present <- function(x) if (length(x) == 0) list() else x
    state <<- list(
      wire = function(repeats) {
        tbl <- pbench_payload(repeats)
        m <- RProtoBuf::new(scalars)
        m$i32 <- tbl$i32
        m$i64 <- tbl$i64
        m$u32 <- tbl$u32
        # R has no uint64: set the max from its decimal string, which
        # serializes to the same 10-byte varint as the bit pattern -1.
        m$u64 <- "18446744073709551615"
        m$s32 <- tbl$s32
        m$s64 <- tbl$s64
        m$fx32 <- tbl$fx32
        m$fx64 <- tbl$fx64
        m$sfx32 <- tbl$sfx32
        m$sfx64 <- tbl$sfx64
        m$f32 <- tbl$f32
        m$f64 <- tbl$f64
        m$flag <- tbl$flag
        m$str <- tbl$str
        m$color <- tbl$color
        m$inner <- RProtoBuf::new(inner, name = tbl$inner$name,
                                  value = tbl$inner$value)
        m$opt_i32 <- tbl$opt_i32
        m$opt_str <- tbl$opt_str
        m$nums <- tbl$nums
        m$tags <- tbl$tags
        m$inners <- lapply(tbl$inners,
                           function(x) RProtoBuf::new(inner, name = x$name,
                                                      value = x$value))
        m$flags <- tbl$flags
        RProtoBuf::serialize(m, NULL)
      },
      # The same materialization benchmarks/bench.R compares against: one
      # plain-list read per field. `[[` avoids `str` colliding with the
      # Message method of the same name, and absent repeated fields become
      # empty lists exactly like pbwire's decode.
      materialize = function(wire) {
        m <- RProtoBuf::read(scalars, wire)
        list(i32 = m[["i32"]], i64 = m[["i64"]], u32 = m[["u32"]],
             u64 = m[["u64"]], s32 = m[["s32"]], s64 = m[["s64"]],
             fx32 = m[["fx32"]], fx64 = m[["fx64"]],
             sfx32 = m[["sfx32"]], sfx64 = m[["sfx64"]],
             f32 = m[["f32"]], f64 = m[["f64"]], flag = m[["flag"]],
             str = m[["str"]], color = m[["color"]],
             inner = mat_inner(m[["inner"]]),
             opt_i32 = m[["opt_i32"]], opt_str = m[["opt_str"]],
             nums = present(m[["nums"]]), tags = present(m[["tags"]]),
             inners = present(lapply(m[["inners"]], mat_inner)),
             flags = present(m[["flags"]]))
      }
    )
    state
  }
})

# A pbwire named list compared against the baseline materialization: `u64` has
# no exact R spelling on either side (int64 past 2^53), so it is compared
# through the round trip instead, exactly like benchmarks/bench.R.
pbench_same_as_baseline <- function(back, wire) {
  mine <- back
  got <- pbench_baseline()$materialize(wire)
  mine$u64 <- NULL
  got$u64 <- NULL
  identical(got[names(mine)], mine)
}
