PHP_ARG_ENABLE(parallelx, whether to enable parallelx support,
[  --enable-parallelx   Enable parallelx extension], yes)

if test "$PHP_PARALLELX" != "no"; then
  PHP_SUBST(PARALLELX_SHARED_LIBADD)
  AC_DEFINE(HAVE_PARALLELX, 1, [Have parallelx])
  AC_MSG_NOTICE([building parallelx])
  PHP_NEW_EXTENSION(parallelx, src/parallelx.c src/parallelx.h, $ext_shared)
fi
