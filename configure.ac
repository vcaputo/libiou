AC_INIT([libiou], [0.1], [vcaputo@pengaru.com])
AM_INIT_AUTOMAKE([-Wall -Werror foreign])
AC_PROG_CC
AM_PROG_CC_C_O
AM_PROG_AR
AC_PROG_RANLIB
AM_SILENT_RULES([yes])

CFLAGS="$CFLAGS -Wall"

dnl Check for liburing
PKG_CHECK_MODULES(URING, liburing)
CFLAGS="$CFLAGS $URING_CFLAGS"
LIBS="$LIBS $URING_LIBS"

AC_CONFIG_FILES([
 Makefile
 src/Makefile
])
AC_OUTPUT
