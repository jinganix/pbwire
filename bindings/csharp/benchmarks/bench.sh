#!/bin/sh
# Benchmarks the C# binding against the official Google.Protobuf C# runtime and
# generated code, plus protobuf-net (the fastest widely used C# runtime), on
# the same matrix3.proto (the four standard tiers from BENCHMARKS.md).
#
# Needs protoc and the .NET SDK; `dotnet restore` fetches Google.Protobuf and
# protobuf-net (NuGet) on the first run. The generated code is compiled into
# the benchmark project, so no protoc plugin is involved; protobuf-net uses
# hand-written contracts in Pbn.cs matching matrix3.proto field for field.
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

exec dotnet run --project "$here/Bench.csproj" -c Release --no-launch-profile -- "$gen/matrix3.desc"
