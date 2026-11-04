#!/bin/sh
# Benchmarks the Node.js binding against google-protobuf (jspb) generated code
# and protobufjs (the fastest widely used JS runtime) on the same matrix3.proto
# (the four standard tiers from BENCHMARKS.md). protobufjs loads matrix3.proto
# at run time, so it needs no codegen step.
#
# Needs Node, a C compiler and protoc; npm-installs google-protobuf (the
# runtime), protoc-gen-js (the generator) and protobufjs into
# benchmarks/node_modules on first run, and downloads nothing else.
#
#   ./bench.sh          build and run
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
version=${GOOGLE_PROTOBUF_VERSION:-4.0.3}
proto=$root/bindings/c/tests/data/matrix3.proto
gen=$here/gen

if [ ! -x "$here/node_modules/.bin/protoc-gen-js" ] || [ ! -d "$here/node_modules/protobufjs" ]; then
    echo "installing google-protobuf $version + protobufjs into $here/node_modules"
    npm install --prefix "$here" --no-save --no-package-lock --no-audit --no-fund \
        "google-protobuf@$version" "protoc-gen-js@$version" protobufjs
fi

"$root/bindings/node/build.sh" build

mkdir -p "$gen"
protoc -I"$(dirname "$proto")" \
    --plugin=protoc-gen-js="$here/node_modules/.bin/protoc-gen-js" \
    --js_out=import_style=commonjs,binary:"$gen" \
    --descriptor_set_out="$gen/matrix3.desc" --include_imports "$proto"

exec env NODE_PATH="$here/node_modules" node "$here/bench.js"
