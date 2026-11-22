# shim arm: the generated model's shape, one .Call per op. `Structtest_Scalars3_encode`
# hands the whole named list to C, which walks it and fills the tree inside C and
# returns the raw vector; `_decode` hands the wire bytes over and gets the fully
# materialized named list back. The only difference from the generated model is
# that the type is resolved once (`pbwire_type_number`) instead of being passed
# as a name string on every call.
#
# Documented-eager (cf. the Ruby arm): no fold-sink is needed on decode.
# `pbwr_decode_number` runs `pbwr_decode_desc`, which decodes into a tree and
# then returns `pbwr_message_sexp(msg, s, 0)` -- that walk builds every
# repeated R vector element by element (`pbwr_repeated_sexp` loops over all
# `pbw_value_count` items, recursing into every nested message) and converts
# every scalar (`pbwr_scalar_sexp`) before returning (see runtime/src/
# pbwire_r.c). The full R list therefore exists when the timed call returns;
# a fold over the result would only add R getter calls on top of finished C
# work. Encode reuses the app-owned list across iterations (the steady-state
# carve-out, disclosed here and in the README).
#
#   shim_encode(pbwire list) -> wire bytes
#   shim_decode(wire)        -> pbwire list

shim_encode <- function(msg) {
  pbwire_encode_number(pbench_schema(), pbench_message_number(), msg)
}

shim_decode <- function(wire) {
  pbwire_decode_number(pbench_schema(), pbench_message_number(), wire)
}
