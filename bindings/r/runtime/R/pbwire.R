# R wrappers over the pbwire .Call entry points (see src/pbwire_r.c).
# Messages are named lists; the schema is an external pointer holding one
# pbw_ctx (arena + encode buffer, reset per call). Every .Call scopes to
# PACKAGE = "pbwire": a bare name re-searches every loaded DLL per call,
# which costs 30+ us once a native-heavy package (RProtoBuf, Rcpp) is in the
# session; the registered table hit is ~100 ns.

pbwire_load_raw <- function(data, limits = NULL) {
  .Call("pbwr_load", data, limits, PACKAGE = "pbwire")
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

pbwire_decode <- function(schema, type, wire, limits = NULL) {
  .Call("pbwr_decode", schema, type, wire, limits, PACKAGE = "pbwire")
}

pbwire_roundtrip <- function(schema, type, wire) {
  .Call("pbwr_roundtrip", schema, type, wire, PACKAGE = "pbwire")
}

pbwire_to_json <- function(schema, type, msg) {
  .Call("pbwr_to_json", schema, type, msg, PACKAGE = "pbwire")
}

pbwire_from_json <- function(schema, type, text, ignore = FALSE, limits = NULL) {
  .Call("pbwr_from_json", schema, type, text, ignore, limits, PACKAGE = "pbwire")
}

pbwire_to_text <- function(schema, type, msg) {
  .Call("pbwr_to_text", schema, type, msg, PACKAGE = "pbwire")
}

pbwire_from_text <- function(schema, type, text, limits = NULL) {
  .Call("pbwr_from_text", schema, type, text, limits, PACKAGE = "pbwire")
}

pbwire_encode_deterministic <- function(schema, type, msg) {
  .Call("pbwr_encode_deterministic", schema, type, msg, PACKAGE = "pbwire")
}

pbwire_encode_delimited <- function(schema, type, msg) {
  .Call("pbwr_encode_delimited", schema, type, msg, PACKAGE = "pbwire")
}

pbwire_decode_delimited <- function(schema, type, wire, limits = NULL) {
  .Call("pbwr_decode_delimited", schema, type, wire, limits, PACKAGE = "pbwire")
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

pbwire_parse_json <- function(schema, type, text, ignore = FALSE, limits = NULL) {
  .Call("pbwr_parse_json", schema, type, text, ignore, limits, PACKAGE = "pbwire")
}

pbwire_parse_text <- function(schema, type, text, limits = NULL) {
  .Call("pbwr_parse_text", schema, type, text, limits, PACKAGE = "pbwire")
}

pbwire_parse_binary <- function(schema, type, wire, limits = NULL) {
  .Call("pbwr_parse_binary", schema, type, wire, limits, PACKAGE = "pbwire")
}

pbwire_msg_to_json <- function(msg) {
  .Call("pbwr_msg_to_json", msg, PACKAGE = "pbwire")
}

pbwire_msg_to_text <- function(msg, print_unknown = TRUE) {
  .Call("pbwr_msg_to_text", msg, print_unknown, PACKAGE = "pbwire")
}

pbwire_msg_encode <- pbwire_message_encode

## Single-level scan of wire bytes: a list of list(number, wire_type, value)
## with no message tree. Scalars arrive as doubles, LEN payloads as raw.
pbwire_visit <- function(schema, type, wire, limits = NULL) {
  .Call("pbwr_visit", schema, type, wire, limits, PACKAGE = "pbwire")
}

## Exact number of bytes the wire encoding writes.
pbwire_encoded_size <- function(schema, type, msg) {
  .Call("pbwr_encoded_size", schema, type, msg, PACKAGE = "pbwire")
}

## Encode into a fixed cap (size + 12 always fits); too small fails.
pbwire_encode_to <- function(schema, type, msg, cap) {
  .Call("pbwr_encode_to", schema, type, msg, cap, PACKAGE = "pbwire")
}

pbwire_encode_delimited_to <- function(schema, type, msg, cap) {
  .Call("pbwr_encode_delimited_to", schema, type, msg, cap, PACKAGE = "pbwire")
}

## Encode and hand the bytes to `fn` in `chunk`-byte pieces; `fn` aborts by
## returning FALSE. shortcut: chunks a full encode instead of streaming from
## the core; fine until messages outgrow memory.
pbwire_encode_sink <- function(schema, type, msg, fn, chunk = 8192L) {
  wire <- pbwire_encode(schema, type, msg)
  n <- length(wire)
  if (n == 0L) {
    return(invisible(isTRUE(fn(raw(0)))))
  }
  chunk <- as.integer(chunk)
  if (is.na(chunk) || chunk <= 0L) {
    stop("pbwire: encode_sink: chunk must be positive")
  }
  i <- 1L
  while (i <= n) {
    j <- min(n, i + chunk - 1L)
    if (!isTRUE(fn(wire[i:j]))) {
      stop("pbwire: encode_sink: sink aborted")
    }
    i <- j + 1L
  }
  invisible(TRUE)
}

## Loaded service names, for routing.
pbwire_services <- function(schema) {
  .Call("pbwr_services", schema, PACKAGE = "pbwire")
}

## One service and its methods (streaming flags included).
pbwire_service <- function(schema, name) {
  .Call("pbwr_service", schema, name, PACKAGE = "pbwire")
}

## Resolve a method to its input/output message names.
pbwire_resolve <- function(schema, service, method) {
  .Call("pbwr_resolve", schema, service, method, PACKAGE = "pbwire")
}

## Pack already-encoded payload into an Any builder.
pbwire_any_pack <- function(schema, type, type_url, payload) {
  .Call("pbwr_any_pack", schema, type, type_url, payload, PACKAGE = "pbwire")
}

pbwire_timestamp_from_millis <- function(schema, type, ms) {
  .Call("pbwr_timestamp_from_millis", schema, type, ms, PACKAGE = "pbwire")
}

pbwire_duration_from_millis <- function(schema, type, ms) {
  .Call("pbwr_duration_from_millis", schema, type, ms, PACKAGE = "pbwire")
}

pbwire_msg_encoded_size <- function(msg) {
  .Call("pbwr_msg_encoded_size", msg, PACKAGE = "pbwire")
}

pbwire_msg_any_type_url <- function(msg) {
  .Call("pbwr_msg_any_type_url", msg, PACKAGE = "pbwire")
}

pbwire_msg_any_payload <- function(msg) {
  .Call("pbwr_msg_any_payload", msg, PACKAGE = "pbwire")
}

pbwire_msg_timestamp_millis <- function(msg) {
  .Call("pbwr_msg_timestamp_millis", msg, PACKAGE = "pbwire")
}

pbwire_msg_duration_millis <- function(msg) {
  .Call("pbwr_msg_duration_millis", msg, PACKAGE = "pbwire")
}

pbwire_msg_fieldmask_paths <- function(msg) {
  .Call("pbwr_msg_fieldmask_paths", msg, PACKAGE = "pbwire")
}

## Bounded freelist of contexts: acquire instead of new, release instead of
## reset/free. A released context is already reset.
pbwire_new_pool <- function(max_idle = 0, retain_cap = 0) {
  .Call("pbwr_new_pool", max_idle, retain_cap, PACKAGE = "pbwire")
}

pbwire_pool_acquire <- function(pool) {
  .Call("pbwr_pool_acquire", pool, PACKAGE = "pbwire")
}

pbwire_pool_release <- function(pool, ctx) {
  .Call("pbwr_pool_release", pool, ctx, PACKAGE = "pbwire")
  invisible(NULL)
}

pbwire_pool_idle <- function(pool) {
  .Call("pbwr_pool_idle", pool, PACKAGE = "pbwire")
}

pbwire_new_context <- function() {
  .Call("pbwr_new_context", PACKAGE = "pbwire")
}

pbwire_context_reset <- function(ctx) {
  .Call("pbwr_context_reset", ctx, PACKAGE = "pbwire")
  invisible(NULL)
}

pbwire_context_used <- function(ctx) {
  .Call("pbwr_context_used", ctx, PACKAGE = "pbwire")
}

pbwire_context_retain_cap <- function(ctx, cap = NULL) {
  if (is.null(cap)) {
    .Call("pbwr_context_retain_cap", ctx, cap, PACKAGE = "pbwire")
  } else {
    invisible(.Call("pbwr_context_retain_cap", ctx, cap, PACKAGE = "pbwire"))
  }
}

pbwire_context_decode <- function(ctx, schema, type, wire, limits = NULL) {
  .Call("pbwr_context_decode", ctx, schema, type, wire, limits, PACKAGE = "pbwire")
}

pbwire_context_encode <- function(ctx, schema, type, msg) {
  .Call("pbwr_context_encode", ctx, schema, type, msg, PACKAGE = "pbwire")
}
