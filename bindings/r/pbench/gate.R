# The pbench gate: no arm is timed before it reproduces RProtoBuf's bytes on
# all four tiers and reads its decode back field by field. Runs first and stays
# runnable:
#
#   Rscript bindings/r/pbench/gate.R
#
# Writes `gate: PASS` and exits 0, or names the failing arm/tier and exits 1.

source(file.path(dirname(sub(
  "^--file=", "",
  grep("^--file=", commandArgs(trailingOnly = FALSE), value = TRUE)[[1]]
)), "payload.R"))

failures <- 0L

check <- function(ok, what) {
  if (isTRUE(ok)) {
    return(invisible(TRUE))
  }
  cat("FAIL", what, "\n")
  failures <<- failures + 1L
  invisible(FALSE)
}

# Both arms must exist before anything is timed: the tree arm is the prebuilt
# builder handle, the shim arm the one-.Call list path.
arms <- list(
  list(name = "tree", file = "a-tree.R", need = c("tree_encode", "tree_decode")),
  list(name = "shim", file = "b-shim.R", need = c("shim_encode", "shim_decode"))
)
for (arm in arms) {
  if (!file.exists(file.path(pbench_dir(), arm$file))) {
    check(FALSE, paste0(arm$name, " arm missing: ", arm$file, " (",
                        paste(arm$need, collapse = ", "), ")"))
  }
}
if (failures > 0) {
  cat(failures, "gate check(s) failed\n")
  quit(status = 1)
}
for (arm in arms) {
  source(file.path(pbench_dir(), arm$file))
}

baseline <- pbench_baseline()
msg_no <- pbench_message_number()

for (tier in PB_TIERS) {
  label <- tier$label
  repeats <- tier$repeats
  ref <- baseline$wire(repeats)
  if (!check(length(ref) == tier$bytes,
             sprintf("%s: RProtoBuf wrote %d bytes, expected %d", label,
                     length(ref), tier$bytes))) {
    next
  }
  fix <- pbench_payload(repeats)

  # tree: the shipped per-field full task through the builder handle. Encode
  # builds a fresh builder per call (message + set + encode); decode parses
  # to a builder and reads it fully back (JSON print, the shipped read-back
  # path: builders have no per-field getters by design).
  check(identical(tree_encode_full(fix), ref),
        paste(label, ": tree bytes differ"))
  check(identical(tree_decode_full(ref),
                   pbwire_msg_to_json(tree_prepare(fix))),
        paste(label, ": tree decode does not print like the source tree"))
  tree_back <- tree_decode(ref)
  check(identical(tree_encode(tree_back), ref),
        paste(label, ": tree re-encode differs"))

  # shim: the named list in and out through one .Call.
  check(identical(shim_encode(fix), ref),
        paste(label, ": shim bytes differ from RProtoBuf"))
  shim_back <- shim_decode(ref)
  check(pbench_reads_ok(shim_back, repeats),
        paste(label, ": shim decode differs"))
  check(identical(shim_encode(shim_back), ref),
        paste(label, ": shim re-encode differs"))
  # The same materialization benchmarks/bench.R compares against, so a decode
  # that only re-encodes correctly does not pass.
  check(pbench_same_as_baseline(shim_back, ref),
        paste(label, ": shim decode differs from the RProtoBuf list"))

  cat(sprintf("OK   %s: %d bytes, tree/shim equal, x%d\n", label, length(ref),
              repeats))
}

if (failures == 0L) {
  cat("gate: PASS\n")
} else {
  cat(failures, "gate check(s) failed\n")
  quit(status = 1)
}
