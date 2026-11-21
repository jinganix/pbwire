// The pure-Swift wire core (plan 21): a descriptor reader, a value tree, wire
// primitives and a binary encoder/decoder. No C module, no `libpbwire.a`, no
// `unsafeFlags` -- that is the whole point of the product: a SwiftPM consumer
// can depend on it, and a build with no C toolchain can use it.
//
// The implementation lands in stage 3; the gate (Tests/PbwireCoreTests) is red
// until it does.
