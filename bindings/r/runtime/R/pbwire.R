# R wrappers over the pbwire .Call entry points (see src/pbwire_r.c).
# Messages are named lists; the schema is an external pointer holding one
# pbw_ctx (arena + encode buffer, reset per call). Every .Call scopes to
# PACKAGE = "pbwire": a bare name re-searches every loaded DLL per call,
# which costs 30+ us once a native-heavy package (RProtoBuf, Rcpp) is in the
# session; the registered table hit is ~100 ns.

pbwire_load_raw <- function(data) {
  .Call("pbwr_load", data, PACKAGE = "pbwire")
}

pbwire_load <- function(path) {
  if (is.raw(path)) {
    return(pbwire_load_raw(path))
  }
  size <- file.info(path)$size
  if (is.na(size)) {
    stop(paste0("pbwire: cannot open ", path))
  }
  con <- file(path, "rb")
  on.exit(close(con))
  pbwire_load_raw(readBin(con, "raw", n = size))
}

pbwire_loadfile <- pbwire_load

pbwire_encode <- function(schema, type, msg) {
  .Call("pbwr_encode", schema, type, msg, PACKAGE = "pbwire")
}

pbwire_decode <- function(schema, type, wire) {
  .Call("pbwr_decode", schema, type, wire, PACKAGE = "pbwire")
}

pbwire_roundtrip <- function(schema, type, wire) {
  .Call("pbwr_roundtrip", schema, type, wire, PACKAGE = "pbwire")
}

pbwire_to_json <- function(schema, type, msg) {
  .Call("pbwr_to_json", schema, type, msg, PACKAGE = "pbwire")
}

pbwire_from_json <- function(schema, type, text, ignore = FALSE) {
  .Call("pbwr_from_json", schema, type, text, ignore, PACKAGE = "pbwire")
}

pbwire_to_text <- function(schema, type, msg) {
  .Call("pbwr_to_text", schema, type, msg, PACKAGE = "pbwire")
}

pbwire_from_text <- function(schema, type, text) {
  .Call("pbwr_from_text", schema, type, text, PACKAGE = "pbwire")
}

pbwire_encode_deterministic <- function(schema, type, msg) {
  .Call("pbwr_encode_deterministic", schema, type, msg, PACKAGE = "pbwire")
}

pbwire_encode_delimited <- function(schema, type, msg) {
  .Call("pbwr_encode_delimited", schema, type, msg, PACKAGE = "pbwire")
}

pbwire_decode_delimited <- function(schema, type, wire) {
  .Call("pbwr_decode_delimited", schema, type, wire, PACKAGE = "pbwire")
}

pbwire_message <- function(schema, type) {
  .Call("pbwr_message_new", schema, type, PACKAGE = "pbwire")
}

pbwire_message_set <- function(msg, fields) {
  .Call("pbwr_message_set", msg, fields, PACKAGE = "pbwire")
  invisible(msg)
}

pbwire_msg_set <- pbwire_message_set

pbwire_message_encode <- function(msg) {
  .Call("pbwr_message_encode", msg, PACKAGE = "pbwire")
}

pbwire_parse_json <- function(schema, type, text, ignore = FALSE) {
  .Call("pbwr_parse_json", schema, type, text, ignore, PACKAGE = "pbwire")
}

pbwire_parse_text <- function(schema, type, text) {
  .Call("pbwr_parse_text", schema, type, text, PACKAGE = "pbwire")
}

pbwire_parse_binary <- function(schema, type, wire) {
  .Call("pbwr_parse_binary", schema, type, wire, PACKAGE = "pbwire")
}

pbwire_msg_to_json <- function(msg) {
  .Call("pbwr_msg_to_json", msg, PACKAGE = "pbwire")
}

pbwire_msg_to_text <- function(msg, print_unknown = TRUE) {
  .Call("pbwr_msg_to_text", msg, print_unknown, PACKAGE = "pbwire")
}

pbwire_msg_encode <- pbwire_message_encode
