#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.69])
AC_INIT([imbibe], [0.1.0], [code@clayden.org])

AC_LANG(C)

# Get compiler flags from R
: ${R_HOME=`R RHOME`}
if test -z "${R_HOME}"; then
  echo "could not determine R_HOME"
  exit 1
fi
CC=`"${R_HOME}/bin/R" CMD config CC`
CFLAGS=`"${R_HOME}/bin/R" CMD config CFLAGS`
CPPFLAGS=`"${R_HOME}/bin/R" CMD config CPPFLAGS`
LDFLAGS=`"${R_HOME}/bin/R" CMD config LDFLAGS`

# Checks for programs.
AC_PROG_CC

# Checks for OpenMP.
m4_include([tools/m4/ax_openmp.m4])
AX_OPENMP

AC_SUBST(OPENMP_CFLAGS)

AC_CONFIG_FILES([src/Makevars])
AC_OUTPUT
