# tree arm: the shipped per-field full task through the builder handle
# (the shipped runtime). No prebuilt tree before the timer.
#
#   tree_prepare(msg)     -> fresh builder handle (message + set, the per-field
#                            build: R has no per-field setter, so set() walks
#                            every named field in C, see pbwr_fill in
#                            runtime/src/pbwire_r.c)
#   tree_encode(handle)   -> wire bytes (one builder encode; the proxy op)
#   tree_encode_full(msg) -> wire bytes through a fresh builder per call
#                            (the timed encode: builder alloc + full C field
#                            walk + encode, all inside the timer)
#   tree_decode(wire)     -> builder tree (wire -> tree in C, one .Call;
#                            the proxy op)
#   tree_decode_full(wire)-> the builder fully read back as JSON text (the
#                            timed decode: parse + a full C walk of every
#                            field, the same print the gate asserts identical;
#                            R builders have no per-field getters by design, so
#                            the JSON/text print IS the shipped read-back path)
#
# The R list feeding the encode is app-owned steady-state input (the
# Python-arm precedent: encode is one .Call either way, so a fresh R list
# per iteration would price list construction, not the codec). The fresh
# builder, the whole field walk and the encode all sit inside the timer.
# No manual free exists on this path: the builder is an external pointer
# and R's GC runs the finalizer (same note as the Python/Java/Ruby arms).

tree_prepare <- function(msg) {
  tree <- pbwire_message(pbench_schema(), PB_MSG)
  pbwire_message_set(tree, msg)
  tree
}

tree_encode <- function(tree) {
  pbwire_message_encode(tree)
}

tree_encode_full <- function(msg) {
  tree_encode(tree_prepare(msg))
}

tree_decode <- function(wire) {
  pbwire_parse_binary(pbench_schema(), PB_MSG, wire)
}

tree_decode_full <- function(wire) {
  pbwire_msg_to_json(tree_decode(wire))
}
