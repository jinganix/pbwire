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
  s.license = { :type => "Apache-2.0", :file => "../../LICENSE" }
  s.authors = { "jinganix" => "jinganix@gmail.com" }
  s.source = { :git => "https://github.com/jinganix/pbwire.git", :tag => "v#{s.version}" }
  s.osx.deployment_target = "10.15"
  s.ios.deployment_target = "13.0"
  s.requires_arc = true
  s.source_files = "*.{h,m}", "../../src/*.c"
  s.public_header_files = "*.h", "../../include/pbwire.h"
  s.header_mappings_dir = "../.."
  s.pod_target_xcconfig = { "HEADER_SEARCH_PATHS" => "$(PODS_TARGET_SRCROOT)/../../include" }
end
