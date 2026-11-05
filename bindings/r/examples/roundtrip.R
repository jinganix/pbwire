# The addressbook example through named lists: assign fields, encode,
# decode, compare. The encoded bytes must equal the pinned reference
# examples/addressbook.bin.
#
#   Rscript examples/roundtrip.R [desc] [bin]   (from bindings/r)

here <- dirname(sub("^--file=", "", grep("^--file=", commandArgs(),
                                         value = TRUE)[1L]))
dyn.load(file.path(here, "..", "pbwire.so"))
source(file.path(here, "..", "runtime", "R", "pbwire.R"))

args <- commandArgs(trailingOnly = TRUE)
desc <- if (length(args) >= 1) args[1] else "../../examples/addressbook.desc"
bin <- if (length(args) >= 2) args[2] else "../../examples/addressbook.bin"

phone <- function(number, type) list(number = number, type = type)

build <- function() {
  list(people = list(
    list(name = "Alice", id = 1L, phones = list(
      phone("555-0100", "MOBILE"))),
    list(name = "Bob", id = 2L, email = "bob@example.com", phones = list(
      phone("555-0200", "HOME")))
  ))
}

schema <- pbwire_loadfile(desc)
wire <- pbwire_encode(schema, "tutorial.AddressBook", build())

if (file.exists(bin)) {
  con <- file(bin, "rb")
  ref <- readBin(con, "raw", n = file.info(bin)$size)
  close(con)
  stopifnot(identical(wire, ref))
}

back <- pbwire_decode(schema, "tutorial.AddressBook", wire)
cat(sprintf("addressbook: %d bytes, %d people\n",
            length(wire), length(back$people)))
for (person in back$people) {
  email <- if (!is.null(person$email)) person$email else ""
  cat(sprintf("  %s (%d) %s\n", person$name, person$id, email))
  for (ph in person$phones) {
    cat("   ", ph$number, "\n")
  }
}
