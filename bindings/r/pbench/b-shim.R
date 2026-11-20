# shim arm: the generated model's shape, one .Call per op. `Structtest_Scalars3_encode`
# hands the whole named list to C, which walks it and fills the tree inside C and
# returns the raw vector; `_decode` hands the wire bytes over and gets the fully
# materialized named list back. The only difference from the generated model is
# that the type is resolved once (`pbwire_type_number`) instead of being passed
# as a name string on every call.
#
#   shim_encode(pbwire list) -> wire bytes
#   shim_decode(wire)        -> pbwire list

shim_encode <- function(msg) {
  pbwire_encode_number(pbench_schema(), pbench_message_number(), msg)
}

shim_decode <- function(wire) {
  pbwire_decode_number(pbench_schema(), pbench_message_number(), wire)
}
