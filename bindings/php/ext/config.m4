PHP_ARG_ENABLE([pbwire],
  [whether to enable the pbwire extension],
  [  --enable-pbwire   Enable the pbwire extension])

if test "$PHP_PBWIRE" != "no"; then
  dnl Self-contained like the Ruby C extension: core sources are symlinked
  dnl into this directory (core_*.c) because PHP_NEW_EXTENSION cannot handle
  dnl source paths with directories (object names would collide).
  PHP_NEW_EXTENSION([pbwire],
    [pbwire.c core_wire.c core_arena.c core_schema.c core_message.c core_encode.c core_decode.c core_ctx.c core_descriptor_set.c core_text.c core_json.c core_stream.c],
    [$ext_shared])
  PHP_ADD_INCLUDE([$abs_srcdir/../../../include])
  PHP_ADD_INCLUDE([$abs_srcdir/../../../src])
fi
