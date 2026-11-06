#!/bin/sh
# Runs the R benchmark: install the package, then the four standard tiers.
# Needs only R and a C compiler; with RProtoBuf installed the baseline
# columns are timed too. Building RProtoBuf from source against a modern
# protobuf (36.x) needs `ac_cv_prog_cxx_cxx11= Rscript -e 'install.packages(...)'`:
# its configure appends -std=gnu++11 (its C++11 probe is itself invalid under
# R's default -std=gnu++20), which abseil rejects.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
sh "$here/../build.sh" >/dev/null
cd "$here"
exec ${RSCRIPT:-Rscript} bench.R
