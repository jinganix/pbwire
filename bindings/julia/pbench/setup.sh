#!/bin/sh
# Prepares what the Julia pbench arms need: the generated flat-layout model
# (protoc-gen-pbwire) and the ProtoBuf.jl oracle. Idempotent; nothing is
# committed.
#
#   bindings/julia/pbench/setup.sh
set -eu

here=$(cd "$(dirname "$0")" && pwd)
julia_dir=$(cd "$here/.." && pwd)
root=$(cd "$julia_dir/../.." && pwd)
gen=$here/gen
proto=$root/bindings/c/tests/data/matrix3.proto

make -C "$root/codegen" build >/dev/null
mkdir -p "$gen"
protoc -I"$(dirname "$proto")" \
    --plugin=protoc-gen-pbwire="$root/codegen/build/protoc-gen-pbwire" \
    --pbwire_out="$gen" --pbwire_opt=lang=julia \
    --descriptor_set_out="$gen/matrix3.desc" --include_imports "$proto"

oracle=$julia_dir/benchmarks/oracle/structtest/matrix3_pb.jl
if [ ! -f "$oracle" ]; then
    mkdir -p "$(dirname "$oracle")"
    julia -e 'using ProtoBuf; protojl("matrix3.proto",
        joinpath(ARGS[1], "bindings/c/tests/data"),
        joinpath(ARGS[1], "bindings/julia/benchmarks/oracle"))' "$root"
fi

sh "$julia_dir/build.sh"
