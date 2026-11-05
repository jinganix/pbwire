#!/usr/bin/env Rscript
## Google conformance testee driven by the pbwire R binding.
##
## Speaks the runner's wire protocol (4-byte little-endian length prefix,
## then a ConformanceRequest / ConformanceResponse) and, because the binding
## is schema-driven, needs no per-message code: the request names the message
## type and the descriptor set answers with the schema.
##
## Supported: binary, JSON and text in -> binary, JSON or text out.
## Binary travels byte-preserving through pbwire_roundtrip() (unknown fields
## survive); JSON/text travel through builder trees (exact, including 64-bit
## integers past 2^53 that lists carry as doubles). JSPB answers `skipped`.
##
##   Rscript testee.R build/conformance.desc   # from the repo root

args <- commandArgs(trailingOnly = TRUE)
if (length(args) < 1L) {
  write("usage: testee.R <conformance.desc>", stderr())
  quit(status = 2)
}

here <- dirname(sub("^--file=", "", grep("^--file=", commandArgs(),
                                         value = TRUE)[1L]))
dyn.load(file.path(here, "..", "pbwire.so"))
source(file.path(here, "..", "runtime", "R", "pbwire.R"))

schema <- pbwire_load(args[[1L]])
PROTOBUF <- 1L
JSON <- 2L
JSPB <- 3L
TEXT <- 4L

## Test categories the runner sends: JSON input with unknown fields only
## parses when the category asks to ignore them.
JSON_IGNORE_UNKNOWN <- 3L

con <- file("stdin", "rb")

read_n <- function(n) {
  chunks <- list()
  got <- 0L
  while (got < n) {
    piece <- readBin(con, "raw", n - got)
    if (length(piece) == 0L) return(NULL)
    got <- got + length(piece)
    chunks[[length(chunks) + 1L]] <- piece
  }
  do.call(c, chunks)
}

read_u32le <- function(b) {
  sum(as.integer(b) * c(1L, 256L, 65536L, 16777216L))
}

write_u32le <- function(n) {
  as.raw(c(n %% 256L, (n %/% 256L) %% 256L,
           (n %/% 65536L) %% 256L, (n %/% 16777216L) %% 256L))
}

out_con <- file("/dev/stdout", "wb")
respond <- function(payload) {
  writeBin(c(write_u32le(length(payload)), payload), out_con)
  flush(out_con)
}

repeat {
  header <- read_n(4L)
  if (is.null(header)) break
  body <- read_n(read_u32le(header))
  if (is.null(body)) break

  request <- tryCatch(pbwire_decode(schema, "conformance.ConformanceRequest",
                                    body),
                      error = function(e) NULL)
  if (is.null(request)) {
    response <- list(runtime_error = "cannot decode request")
  } else if (is.null(request$requested_output_format) ||
             !(request$requested_output_format %in% c(PROTOBUF, JSON, TEXT))) {
    response <- list(skipped = paste("output format",
                                     request$requested_output_format,
                                     "is not supported"))
  } else {
    wanted <- request$requested_output_format
    ttype <- request$message_type
    ## One input payload wins: binary beats JSON beats text. Builders hold
    ## the parsed tree; tables would lose 64-bit integers past 2^53.
    parsed <- tryCatch({
      if (!is.null(request$protobuf_payload)) {
        if (wanted == PROTOBUF) {
          list(kind = "bytes",
               wire = pbwire_roundtrip(schema, ttype,
                                       request$protobuf_payload))
        } else {
          list(kind = "builder",
               msg = pbwire_parse_binary(schema, ttype,
                                         request$protobuf_payload))
        }
      } else if (!is.null(request$json_payload)) {
        ignore <- isTRUE(request$test_category == JSON_IGNORE_UNKNOWN)
        list(kind = "builder",
             msg = pbwire_parse_json(schema, ttype, request$json_payload,
                                     ignore))
      } else if (!is.null(request$text_payload)) {
        list(kind = "builder",
             msg = pbwire_parse_text(schema, ttype, request$text_payload))
      } else {
        stop("pbwire: no input payload in the request")
      }
    }, error = function(e) e)
    if (inherits(parsed, "error")) {
      m <- conditionMessage(parsed)
      if (grepl("no message", m)) {
        response <- list(runtime_error = paste("unknown message type", ttype))
      } else {
        response <- list(parse_error = m)
      }
    } else if (parsed$kind == "bytes") {
      response <- list(protobuf_payload = parsed$wire)
    } else {
      out <- tryCatch({
        if (wanted == PROTOBUF) {
          list(protobuf_payload = pbwire_msg_encode(parsed$msg))
        } else if (wanted == JSON) {
          list(json_payload = pbwire_msg_to_json(parsed$msg))
        } else {
          ## The runner's print_unknown_fields flag decides whether unknown
          ## fields print; absent (NULL) means drop them.
          print_unknown <- isTRUE(request$print_unknown_fields)
          list(text_payload = pbwire_msg_to_text(parsed$msg, print_unknown))
        }
      }, error = function(e) e)
      if (inherits(out, "error")) {
        response <- list(serialize_error = conditionMessage(out))
      } else {
        response <- out
      }
    }
  }

  ## oneof response fields: represent the answer as the live member only.
  payload <- tryCatch(pbwire_encode(schema, "conformance.ConformanceResponse",
                                    response),
                      error = function(e) pbwire_encode(
                        schema, "conformance.ConformanceResponse",
                        list(runtime_error = "cannot encode response")))
  respond(payload)
}
