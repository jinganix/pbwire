# PBWire podspec: ARC wrapper over the pbwire C core (Foundation only).
# The version is read from the repo-root VERSION file: never hardcode it here.
version = File.read(File.expand_path("../../VERSION", __dir__)).strip

Pod::Spec.new do |s|
  s.name = "PBWire"
  s.version = version
  s.summary = "Dynamic protobuf over the pbwire C ABI, schema at runtime"
  s.description = "A FileDescriptorSet becomes the schema at runtime; messages go " \
                  "to/from wire bytes with no generated code, through an ARC wrapper " \
                  "over the pbwire C core."
  s.homepage = "https://github.com/jinganix/pbwire"
  s.license = { :type => "Apache-2.0", :file => "LICENSE" }
  s.authors = { "jinganix" => "jinganix@gmail.com" }
  s.source = { :git => "https://github.com/jinganix/pbwire.git", :tag => "v#{s.version}" }
  s.osx.deployment_target = "12.0"
  s.ios.deployment_target = "15.0"
  s.requires_arc = true
  # All patterns are resolved from the repo root (the download root), not
  # from this file's directory.
  # internal.h travels with the C sources (they include it by quote); src/protoc
  # stays out: the single-level glob does not descend into it.
  s.source_files = "bindings/objc/*.{h,m}", "src/*.{c,h}", "include/pbwire.h"
  s.public_header_files = "bindings/objc/*.h", "include/pbwire.h"
  s.pod_target_xcconfig = { "HEADER_SEARCH_PATHS" => "$(PODS_TARGET_SRCROOT)/include" }
end
