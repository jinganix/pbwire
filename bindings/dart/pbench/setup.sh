#!/bin/sh
# Prepares what the Dart pbench arms need: the generated flat-layout model
# (protoc-gen-pbwire), the official protoc-gen-dart model, matrix3.desc and the
# native shim library. Idempotent; nothing is committed.
#
#   bindings/dart/pbench/setup.sh
set -eu

here=$(cd "$(dirname "$0")" && pwd)
dart_dir=$(cd "$here/.." && pwd)
root=$(cd "$dart_dir/../.." && pwd)
gen=$here/gen
proto=$root/bindings/c/tests/data/matrix3.proto

if ! command -v protoc-gen-dart >/dev/null 2>&1; then
    echo "setup.sh: protoc-gen-dart not found (dart pub global activate protoc_plugin," \
        "and add ~/.pub-cache/bin to PATH)" >&2
    exit 1
fi

make -C "$root/codegen" build >/dev/null
mkdir -p "$gen"
protoc -I"$(dirname "$proto")" \
    --plugin=protoc-gen-pbwire="$root/codegen/build/protoc-gen-pbwire" \
    --pbwire_out="$gen" --pbwire_opt=lang=dart \
    --dart_out="$gen" \
    --descriptor_set_out="$gen/matrix3.desc" --include_imports "$proto"

sh "$dart_dir/build.sh"
cd "$here"
dart pub get >/dev/null
