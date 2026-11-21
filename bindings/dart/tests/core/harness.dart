// The minimal check harness the pure-core tests share: plain asserts, no
// package:test (this package's dependency set stays ffi/fixnum/protobuf, like
// the binding it ships beside).

import 'dart:io';

int _checks = 0;
int failures = 0;

/// One assertion: prints the failure and keeps going, so one run reports
/// every broken fact instead of the first.
void check(bool ok, String what) {
  _checks++;
  if (!ok) {
    failures++;
    stderr.writeln('FAIL $what');
  }
}

/// Exits non-zero when anything failed; prints the pass count otherwise.
void done(String suite) {
  if (failures != 0) {
    stderr.writeln('$suite: $failures of $_checks checks failed');
    exit(1);
  }
  stdout.writeln('$suite: PASS ($_checks checks)');
}
