# Swift binding conformance

No Swift testee: the official suite drives the C core through
`conformance/testee.c` (`make conformance` at the repo root), and this
binding adds no wire behavior of its own — every byte it encodes or decodes
goes through the same `pbw_encode` / `pbw_decode` the testee exercises. A
Swift testee would test the same core code a second time, which is why the
Objective-C binding skips its testee for the same reason.

What the suite therefore proves for this binding:

- binary wire format: covered end to end by `conformance/testee.c`;
- JSON and text mappings: the binding does not expose `pbw_json_*` /
  `pbw_text_*` wrappers yet, so there is nothing of its own to cover. Call
  the C ABI directly if you need them.

The binding's own gate is `swift test`: descriptor load, a full scalar and
nested round trip, byte-exact re-encode (including one pinned encoding, so
wire drift fails fast), presence, repeated fields, API errors, garbage
rejection, and a lifetime loop.
