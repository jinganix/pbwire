#!/bin/sh
# A/B two builds of the PHP extension from the same tree: base A and candidate
# B are separate .so files, alternated in one session so drift hits both sides.
#
#   ./bindings/php/pbench/ab.sh <base.so> <candidate.so> [rounds] [tier]
#
# Prints ns/op per round; a build whose bytes differ aborts ab.php.
set -eu

base=${1:?usage: ab.sh <base.so> <candidate.so> [rounds] [tier]}
cand=${2:?usage: ab.sh <base.so> <candidate.so> [rounds] [tier]}
rounds=${3:-5}
tier=${4:-xlarge}
here=$(cd "$(dirname "$0")" && pwd)
php=${PHP:-php}

run() {
    "$php" -d "extension=$1" "$here/ab.php" "$tier"
}

printf 'round base-enc base-dec cand-enc cand-dec\n'
for round in $(seq 1 "$rounds"); do
    set -- $(run "$base")
    be=$1
    bd=$2
    set -- $(run "$cand")
    ce=$1
    cd=$2
    printf '%s %s %s %s %s\n' "$round" "$be" "$bd" "$ce" "$cd"
done
