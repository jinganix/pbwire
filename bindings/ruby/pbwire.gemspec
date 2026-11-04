# frozen_string_literal: true

# Gem metadata for the Ruby binding. The version is read from the repo-root
# VERSION file: never hardcode it here.
version = File.read(File.expand_path("../../VERSION", __dir__)).strip

Gem::Specification.new do |s|
  s.name = "pbwire"
  s.version = version
  s.summary = "Dynamic protobuf over the pbwire C ABI, schema at runtime"
  s.description = "A FileDescriptorSet becomes the schema at runtime; messages go " \
                  "to/from wire bytes with no generated code, through a C extension " \
                  "over the pbwire core."
  s.authors = ["jinganix"]
  s.homepage = "https://github.com/jinganix/pbwire"
  s.license = "Apache-2.0"
  s.required_ruby_version = ">= 3.0"
  s.files = Dir["lib/**/*.rb", "ext/**/*.{c,h,rb}", "../../include/pbwire.h",
                "../../src/*.c", "../../VERSION", "README.md"]
  s.require_paths = ["lib"]
  s.extensions = ["ext/pbwire_ext/extconf.rb"]
end
