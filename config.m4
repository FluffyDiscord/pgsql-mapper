dnl config.m4 for pgsql_mapper extension

PHP_ARG_ENABLE([pgsql_mapper],
  [whether to enable pgsql_mapper support],
  [AS_HELP_STRING([--enable-pgsql_mapper],
    [Enable pgsql_mapper support])],
  [no])

if test "$PHP_PGSQL_MAPPER" != "no"; then

  dnl Find libpq via pkg-config
  AC_MSG_CHECKING([for libpq via pkg-config])
  if test -x "$PKG_CONFIG" && $PKG_CONFIG --exists libpq 2>/dev/null; then
    LIBPQ_CFLAGS=$($PKG_CONFIG --cflags libpq)
    LIBPQ_LIBS=$($PKG_CONFIG --libs libpq)
    LIBPQ_LIBDIR=$($PKG_CONFIG --variable=libdir libpq)
    AC_MSG_RESULT([found])
  else
    AC_MSG_RESULT([not found, trying pg_config])
    AC_MSG_CHECKING([for libpq via pg_config])
    if test -x "$(command -v pg_config 2>/dev/null)"; then
      LIBPQ_INCDIR=$(pg_config --includedir)
      LIBPQ_LIBDIR=$(pg_config --libdir)
      LIBPQ_CFLAGS="-I$LIBPQ_INCDIR"
      LIBPQ_LIBS="-L$LIBPQ_LIBDIR -lpq"
      AC_MSG_RESULT([found])
    else
      AC_MSG_RESULT([not found, using defaults])
      LIBPQ_CFLAGS="-I/usr/include -I/usr/include/pgsql"
      LIBPQ_LIBS="-lpq"
      LIBPQ_LIBDIR="/usr/lib64"
    fi
  fi

  PHP_EVAL_INCLINE($LIBPQ_CFLAGS)
  PHP_EVAL_LIBLINE($LIBPQ_LIBS, PGSQL_MAPPER_SHARED_LIBADD)

  dnl Make CPPFLAGS visible to AC_CHECK_HEADER
  SAVED_CPPFLAGS="$CPPFLAGS"
  CPPFLAGS="$CPPFLAGS $LIBPQ_CFLAGS $INCLUDES"

  dnl Check that libpq-fe.h exists
  AC_CHECK_HEADER([libpq-fe.h], [], [
    AC_MSG_ERROR([libpq-fe.h not found. Install postgresql-devel.])
  ])

  CPPFLAGS="$SAVED_CPPFLAGS"

  dnl Check that libpq is linkable
  PHP_CHECK_LIBRARY(pq, PQexec,
  [
    AC_DEFINE(HAVE_LIBPQ, 1, [Have libpq])
  ],[
    AC_MSG_ERROR([libpq not found or PQexec missing. Install postgresql-devel.])
  ],[
    $LIBPQ_LIBS
  ])

  PHP_ADD_EXTENSION_DEP(pgsql_mapper, pgsql)
  PHP_ADD_EXTENSION_DEP(pgsql_mapper, date)

  PHP_SUBST(PGSQL_MAPPER_SHARED_LIBADD)
  PHP_NEW_EXTENSION(pgsql_mapper, pgsql_mapper.c, $ext_shared)

  dnl Tier 3: Release build optimizations (skip for debug builds)
  if test "$PHP_DEBUG" = "no"; then
    CFLAGS="$CFLAGS -O3 -fvisibility=hidden -flto"
    LDFLAGS="$LDFLAGS -flto"
  fi
fi
