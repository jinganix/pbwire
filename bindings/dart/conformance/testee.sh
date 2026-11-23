#!/bin/sh
# The conformance runner spawns one executable, so wrap `dart run`: the
# descriptor file arrives as $1 from the runner.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
exec dart run "$here/testee.dart" "$@"
