#!/usr/bin/env Rscript
## Google conformance testee driven by the pbwire R binding.
##
## Speaks the runner's wire protocol (4-byte little-endian length prefix,
## then a ConformanceRequest / ConformanceResponse) and, because the binding
## is schema-driven, needs no per-message code: the request names the message
## type and the descriptor set answers with the schema.
##
## Supported: binary in -> binary out, through pbwire_roundtrip(), which
## decodes and re-encodes in the C core: unknown fields survive, and a request
## is accepted exactly when the core accepts it. JSON/text output needs
## printers the binding does not expose, so those requests answer `skipped`.
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
             request$requested_output_format != PROTOBUF) {
    response <- list(skipped = paste("output format",
                                     request$requested_output_format,
                                     "is not supported"))
  } else if (is.null(request$protobuf_payload)) {
    response <- list(skipped = "only binary input is supported")
  } else {
    out <- tryCatch(pbwire_roundtrip(schema, request$message_type,
                                     request$protobuf_payload),
                    error = function(e) e)
    if (inherits(out, "error")) {
      m <- conditionMessage(out)
      if (grepl("no message", m)) {
        response <- list(runtime_error = paste("unknown message type",
                                               request$message_type))
      } else {
        response <- list(parse_error = m)
      }
    } else {
      response <- list(protobuf_payload = out)
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
