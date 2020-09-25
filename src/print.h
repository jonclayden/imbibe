#ifndef _PRINT_H_
#define _PRINT_H_

#ifdef USING_R

#define R_NO_REMAP
#define R_USE_C99_IN_CXX

#include <R_ext/Print.h>
#include <R_ext/Error.h>

#define niimath_print(...) Rprintf(__VA_ARGS__)
#define niimath_message(...) REprintf(__VA_ARGS__)

int niimath_rand ();

#else

#include <stdio.h>

#define niimath_print(...) printf(__VA_ARGS__)
#define niimath_message(...) fprintf(stderr, __VA_ARGS__)

#define niimath_rand rand

#endif // USING_R

#endif
