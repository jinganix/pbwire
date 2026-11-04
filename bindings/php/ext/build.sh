#!/bin/sh
# Builds the PHP C extension (self-contained: the C core is compiled in, like
# the Ruby C extension), so bindings/php can run natively instead of through
# ext-ffi. The FFI wrapper stays the default; loading this extension selects
# the native classes (runtime/pbwire.php returns early when it is present).
#
#   ./build.sh            build ext/modules/pbwire.so
set -eu

here=$(cd "$(dirname "$0")" && pwd)

cd "$here"
phpize
./configure
make -j4
echo "built $here/modules/pbwire.so"
