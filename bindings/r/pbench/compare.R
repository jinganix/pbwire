# Provisional interleaved timing of the two R pbench arms against RProtoBuf:
# one round runs every op back to back per tier, five rounds, median reported.
#
#   Rscript bindings/r/pbench/compare.R
#
# The ops are whole user tasks: `tree` encode builds a fresh builder per call
# (message + set + encode) and `tree` decode parses plus reads fully back
# (JSON print, folded into a sink); `shim` encode/decode is one .Call with
# the named list in or out (documented-eager: the C walk in pbwr_decode_desc
# / pbwr_message_sexp materializes every field before returning, so decode
# needs no fold-sink -- see b-shim.R). The baseline is RProtoBuf, materialized
# into a plain list on decode (the same task, see payload.R).

source(file.path(dirname(sub(
  "^--file=", "",
  grep("^--file=", commandArgs(trailingOnly = FALSE), value = TRUE)[[1]]
)), "payload.R"))
source(file.path(pbench_dir(), "a-tree.R"))
source(file.path(pbench_dir(), "b-shim.R"))

ROUNDS <- 5L

# Per-op iteration count: double from one call until a pass takes ~0.2 s.
# Calibrating per op (benchmarks/bench.R does the same) matters here: the arms
# and RProtoBuf are three orders of magnitude apart, so one count per tier would
# spend minutes timing the baseline's 27-ms encode tens of thousands of times.
calibrate <- function(op) {
  count <- 1L
  repeat {
    t0 <- proc.time()[["elapsed"]]
    for (i in seq_len(count)) op()
    if (proc.time()[["elapsed"]] - t0 >= 0.2 || count >= 1e8) break
    count <- count * 2
  }
  count
}

# One timed pass of `count` iterations, ns per op.
pass_ns <- function(op, count) {
  t0 <- proc.time()[["elapsed"]]
  for (i in seq_len(count)) op()
  (proc.time()[["elapsed"]] - t0) / count * 1e9
}

baseline <- pbench_baseline()
medians <- list()

for (tier in PB_TIERS) {
  label <- tier$label
  repeats <- tier$repeats
  fix <- pbench_payload(repeats)
  wire <- shim_encode(fix)
  # The tree decode folds its JSON read-back into a sink so the
  # materialization cannot be skipped; the sink total is printed per tier.
  tree_sink <- new.env(parent = emptyenv())
  tree_sink$n <- 0
  tree_decode_sink <- function() {
    j <- tree_decode_full(wire)
    tree_sink$n <- tree_sink$n + nchar(j)
    invisible(j)
  }
  ops <- list(
    "pbwire-r-tree" = list(encode = function() tree_encode_full(fix),
                           decode = tree_decode_sink),
    "pbwire-r-shim" = list(encode = function() shim_encode(fix),
                           decode = function() shim_decode(wire)),
    "RProtoBuf" = list(encode = function() baseline$wire(repeats),
                       decode = function() baseline$materialize(wire))
  )
  counts <- list()
  for (impl in names(ops)) {
    for (op in c("encode", "decode")) {
      key <- paste(impl, op)
      counts[[key]] <- calibrate(ops[[impl]][[op]])
    }
  }
  samples <- list()
  for (round in seq_len(ROUNDS)) {
    for (impl in names(ops)) {
      for (op in c("encode", "decode")) {
        key <- paste(impl, op)
        samples[[key]] <- c(samples[[key]], pass_ns(ops[[impl]][[op]],
                                                   counts[[key]]))
      }
    }
  }
  for (op in c("encode", "decode")) {
    for (impl in names(ops)) {
      medians[[paste(label, op, impl)]] <-
        median(samples[[paste(impl, op)]])
    }
  }
  cat(sprintf("  # %s tree decode sink: %d chars read back\n", label,
              tree_sink$n))
}

cat("provisional: median of", ROUNDS, "interleaved rounds, ns/op and MB/s\n")
cat(sprintf("  %-8s %-6s %-14s %12s %10s\n", "tier", "op", "impl", "ns/op",
            "MB/s"))
for (tier in PB_TIERS) {
  bytes <- tier$bytes
  for (op in c("encode", "decode")) {
    for (impl in c("pbwire-r-tree", "pbwire-r-shim", "RProtoBuf")) {
      ns <- medians[[paste(tier$label, op, impl)]]
      cat(sprintf("  %-8s %-6s %-14s %12.1f %10.1f\n", tier$label, op, impl,
                  ns, bytes / ns * 1000))
    }
  }
}
