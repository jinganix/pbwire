#!/bin/sh
# Runs the Lua benchmark: build the module, then the four standard tiers.
# Needs only Lua and a C compiler; with lua-protobuf installed (luarocks
# install lua-protobuf) the baseline columns are timed too.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
sh "$here/../build.sh" >/dev/null
cd "$here"
exec ${LUA:-lua} bench.lua
