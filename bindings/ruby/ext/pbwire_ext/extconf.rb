# frozen_string_literal: true

# Builds the C extension for rubygems from the files shipped inside the gem:
# ext/pbwire_ext/pbwire_ext.c plus the vendored core (core_*.c symlinks into
# ../../../../src, packaged as real files) and the two headers the core
# includes. No repo-root paths: `gem install` unpacks elsewhere.
require "mkmf"

core = %w[arena ctx decode descriptor_set encode json message schema stream text wire wkt]
srcs = [File.join(__dir__, "pbwire_ext.c")] + core.map { |c| File.join(__dir__, "core_#{c}.c") }

$INCFLAGS += " -I#{__dir__}"
$srcs = srcs
$objs = srcs.map { |s| File.join(__dir__, "#{File.basename(s, ".c")}.o") }

create_makefile("pbwire_ext")
