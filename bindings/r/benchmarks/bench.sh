#!/bin/sh
# Runs the R benchmark: install the package, then the four standard tiers.
# Needs only R and a C compiler; with RProtoBuf installed the baseline
# columns are timed too.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
sh "$here/../build.sh" >/dev/null
cd "$here"
exec ${RSCRIPT:-Rscript} bench.R
