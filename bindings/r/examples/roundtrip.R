# The addressbook example through the generated constructors: assign fields,
# encode, decode, compare. The encoded bytes must equal the pinned reference
# examples/addressbook.bin.
#
#   Rscript examples/roundtrip_gen.R [desc] [bin]   (from bindings/r)

here <- dirname(sub("^--file=", "", grep("^--file=", commandArgs(),
                                         value = TRUE)[1L]))
dyn.load(file.path(here, "..", "pbwire.so"))
source(file.path(here, "..", "runtime", "R", "pbwire.R"))
source(file.path(here, "gen", "addressbook_meta.R"))
source(file.path(here, "gen", "addressbook_pbwire.R"))

args <- commandArgs(trailingOnly = TRUE)
bin <- if (length(args) >= 1) args[1] else "../../examples/addressbook.bin"

phone <- function(number, type) Tutorial_Person_PhoneNumber_new(number = number, type = type)

build <- function() {
  Tutorial_AddressBook_new(people = list(
    Tutorial_Person_new(name = "Alice", id = 1L, phones = list(
      phone("555-0100", Tutorial_Person_PhoneType_MOBILE))),
    Tutorial_Person_new(name = "Bob", id = 2L, email = "bob@example.com", phones = list(
      phone("555-0200", Tutorial_Person_PhoneType_HOME)))
  ))
}

book <- build()
wire <- Tutorial_AddressBook_encode(book)

if (file.exists(bin)) {
  con <- file(bin, "rb")
  ref <- readBin(con, "raw", n = file.info(bin)$size)
  close(con)
  stopifnot(identical(wire, ref))
}

back <- Tutorial_AddressBook_decode(wire)
cat(sprintf("addressbook: %d bytes, %d people\n",
            length(wire), length(back$people)))
for (person in back$people) {
  email <- if (!is.null(person$email)) person$email else ""
  cat(sprintf("  %s (%d) %s\n", person$name, person$id, email))
  for (ph in person$phones) {
    cat("   ", ph$number, "\n")
  }
}
