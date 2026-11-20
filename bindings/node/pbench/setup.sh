#!/bin/sh
# Prepares what the pbench arms need: the addon, benchmarks/gen/matrix3_pb.js
# (jspb generated code) and benchmarks/gen/matrix3.desc, all in the same
# places benchmarks/bench.sh writes them, so the gate and the bench compare
# the same artifacts. Idempotent; nothing is committed.
#
#   bindings/node/pbench/setup.sh
set -eu

here=$(cd "$(dirname "$0")" && pwd)
node_dir=$(cd "$here/.." && pwd)
root=$(cd "$node_dir/../.." && pwd)
bench=$node_dir/benchmarks
gen=$bench/gen
version=${GOOGLE_PROTOBUF_VERSION:-4.0.3}
proto=$root/bindings/c/tests/data/matrix3.proto

if [ ! -x "$bench/node_modules/.bin/protoc-gen-js" ] || \
   [ ! -d "$bench/node_modules/protobufjs" ]; then
    echo "installing google-protobuf $version + protobufjs into $bench/node_modules"
    npm install --prefix "$bench" --no-save --no-package-lock --no-audit --no-fund \
        "google-protobuf@$version" "protoc-gen-js@$version" protobufjs
fi

"$node_dir/build.sh" build

mkdir -p "$gen"
protoc -I"$(dirname "$proto")" \
    --plugin=protoc-gen-js="$bench/node_modules/.bin/protoc-gen-js" \
    --js_out=import_style=commonjs,binary:"$gen" \
    --descriptor_set_out="$gen/matrix3.desc" --include_imports "$proto"
