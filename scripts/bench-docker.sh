#!/bin/sh
# Run the benchmark suite inside a Linux container and record it next to the
# host records, so Linux rows compare with the macOS tables in BENCHMARKS.md.
#
#   scripts/bench-docker.sh                        # default binding set
#   scripts/bench-docker.sh --bindings core,c      # a subset
#   scripts/bench-docker.sh --rebuild              # rebuild the image first
#   scripts/bench-docker.sh --image my-tag:linux   # use another image
#
# Records land in results/ exactly like the host runs (scripts/bench-run.sh);
# read the pair with scripts/bench-compare.py --latest results.
set -eu

IMAGE=pbwire-bench:linux
DEFAULT_BINDINGS="core,c,go,rust,node,ruby,lua,java,python"
bindings=$DEFAULT_BINDINGS
rebuild=0

while [ $# -gt 0 ]; do
    case "$1" in
        --bindings) bindings="$2"; shift 2 ;;
        --image) IMAGE="$2"; shift 2 ;;
        --rebuild) rebuild=1; shift ;;
        -h|--help) sed -n '2,9p' "$0"; exit 0 ;;
        *) echo "usage: bench-docker.sh [--bindings a,b,c] [--image TAG] [--rebuild]" >&2; exit 2 ;;
    esac
done

# ---- inside the container -------------------------------------------------
if [ "${PBW_BENCH_DOCKER_INNER:-}" = 1 ]; then
    bindings=${PBW_BINDINGS:-$DEFAULT_BINDINGS}
    # The image copied the tree as root, so git refuses it without this (the
    # record's git sha / dirty flag come from it).
    export GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=safe.directory GIT_CONFIG_VALUE_0='*'
    # Where the image put the toolchains; the bench scripts honour these. protoc
    # and libprotobuf live in /usr/local, so PATH/pkg-config need no help.
    export CC=cc NODE_INCLUDE=/usr/include/node LUA_INCDIR=/usr/include/lua5.4
    export PROTOBUF_JAVA_JAR=/opt/deps/protobuf-java-4.36.2.jar
    cd /work

    status=0
    for b in $(echo "$bindings" | tr ',' ' '); do
        case "$b" in
            core) target=bench ;;
            c) target=bench-c ;;
            *) target="$b-bench" ;;
        esac
        echo "== $b (make $target)"
        make PY=python3 RUBY=ruby LUA=lua5.4 "$target" || status=1
    done
    exit "$status"
fi

# ---- outside: build the image, run it, take the records back ---------------
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

if ! command -v docker >/dev/null 2>&1; then
    echo "bench-docker: docker not found" >&2
    exit 1
fi
# Toolchain stage: cached until docker/bench.Dockerfile changes.
if [ "$rebuild" = 1 ] || ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "bench-docker: building $IMAGE (docker/bench.Dockerfile)"
    docker build -f docker/bench.Dockerfile -t "$IMAGE" --target bench .
fi

# Source stage: the working tree under /work, no bind mount (a container with
# no shared host paths would otherwise mount an empty directory and bench
# nothing). stdlib docker cp brings the records back, which also works when
# the daemon lives in another VM. Re-tagging $src each run leaves the previous
# image dangling, which `docker image prune` collects safely.
src=$IMAGE-src
docker build -f docker/bench.Dockerfile -t "$src" --target source . >/dev/null

cid=$(docker create -e PBW_BENCH_DOCKER_INNER=1 -e "PBW_BINDINGS=$bindings" \
    "$src" sh /work/scripts/bench-docker.sh)
set +e
docker start -a "$cid"
status=$?
set -e

mkdir -p results
docker cp "$cid:/work/results/." results/ 2>/dev/null || true
docker rm -f "$cid" >/dev/null
exit "$status"
