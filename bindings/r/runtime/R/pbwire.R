# R wrappers over the pbwire .Call entry points (see src/pbwire_r.c).
# Messages are named lists; the schema is an external pointer holding one
# pbw_ctx (arena + encode buffer, reset per call).

pbwire_load_raw <- function(data) {
  .Call("pbwr_load", data)
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
  .Call("pbwr_encode", schema, type, msg)
}

pbwire_decode <- function(schema, type, wire) {
  .Call("pbwr_decode", schema, type, wire)
}

pbwire_roundtrip <- function(schema, type, wire) {
  .Call("pbwr_roundtrip", schema, type, wire)
}

pbwire_message <- function(schema, type) {
  .Call("pbwr_message_new", schema, type)
}

pbwire_message_set <- function(msg, fields) {
  .Call("pbwr_message_set", msg, fields)
  invisible(msg)
}

pbwire_msg_set <- pbwire_message_set

pbwire_message_encode <- function(msg) {
  .Call("pbwr_message_encode", msg)
}

pbwire_msg_encode <- pbwire_message_encode
