# frozen_string_literal: true

# Builds the C extension for rubygems: compiles ext/pbwire_ext/pbwire_ext.c
# together with the C core (../../../src/*.c), so the gem is self-contained.
require "mkmf"

root = File.expand_path("../../../..", __dir__)
core = %w[wire arena schema message encode decode ctx descriptor_set text json stream]
srcs = [File.join(__dir__, "pbwire_ext.c")] + core.map { |c| File.join(root, "src", "#{c}.c") }

$INCFLAGS += " -I#{File.join(root, "include")}"
$srcs = srcs
$objs = srcs.map { |s| s.sub(/\.c$/, ".o") }

create_makefile("pbwire_ext")
