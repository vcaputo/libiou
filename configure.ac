AC_INIT([libiou], [0.1], [vcaputo@pengaru.com])
AM_INIT_AUTOMAKE([-Wall -Werror foreign])
AC_CONFIG_MACRO_DIRS([m4])
AC_PROG_CC
AM_PROG_CC_C_O
AM_PROG_AR
AC_PROG_RANLIB
AM_SILENT_RULES([yes])

CFLAGS="$CFLAGS -Wall"

AX_PTHREAD
LIBS="$PTHREAD_LIBS $LIBS"
CFLAGS="$CFLAGS $PTHREAD_CFLAGS"
CC="$PTHREAD_CC"

dnl Check for liburing
PKG_CHECK_MODULES(URING, liburing)
CFLAGS="$CFLAGS $URING_CFLAGS"
LIBS="$LIBS $URING_LIBS"

AC_CONFIG_FILES([
 Makefile
 src/Makefile
])
AC_OUTPUT
