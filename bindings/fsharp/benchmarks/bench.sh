#!/bin/sh
# Benchmarks the F# binding against the official Google.Protobuf C# runtime and
# generated code on the same matrix3.proto (the four standard tiers from
# BENCHMARKS.md).
#
# Needs protoc and the .NET SDK; `dotnet restore` fetches Google.Protobuf
# (NuGet) on the first run. The generated code is compiled into the baseline
# project, so no protoc plugin is involved. Unlike the C# harness this one
# skips protobuf-net: it singular-encodes single-element packed fields, so two
# of the four tiers carry nothing MB/s-fair to time.
#
#   ./bench.sh          generate, build, run
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
proto=$root/bindings/c/tests/data/matrix3.proto
gen=$here/gen

mkdir -p "$gen"
protoc -I"$(dirname "$proto")" --csharp_out="$gen" \
    --descriptor_set_out="$gen/matrix3.desc" --include_imports "$proto"

make -C "$root" shared >/dev/null

exec dotnet run --project "$here/Bench.fsproj" -c Release --no-launch-profile -- "$gen/matrix3.desc"
