# tree arm: the prebuilt builder handle (the shipped runtime). The payload is
# loaded into a `pbwire_message` outside the timer -- that is what "the tree
# already exists" means -- and the timed op is one builder encode.
#
#   tree_prepare(pbwire list) -> handle
#   tree_encode(handle)       -> wire bytes
#   tree_decode(wire)         -> builder tree (wire -> tree in C, one .Call)
#
# A builder tree has no field getters in R by design: it is read with
# pbwire_msg_to_json / pbwire_msg_to_text, or re-encoded.

tree_prepare <- function(msg) {
  tree <- pbwire_message(pbench_schema(), PB_MSG)
  pbwire_message_set(tree, msg)
  tree
}

tree_encode <- function(tree) {
  pbwire_message_encode(tree)
}

tree_decode <- function(wire) {
  pbwire_parse_binary(pbench_schema(), PB_MSG, wire)
}
