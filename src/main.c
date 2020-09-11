#include <R.h>
#include <Rdefines.h>

#define RNIFTI_NIFTILIB_VERSION 2
#include "RNifti.h"
#include "RNiftiAPI.h"

#include "core32.h"
#include "core64.h"

SEXP run (SEXP _args, SEXP _precision)
{
    int status = 0;
    int nargs;
    char **args;
    
    PROTECT(_args = AS_CHARACTER(_args));
    nargs = length(_args) + 1;
    args = (char **) R_alloc(nargs, sizeof(char *));
    args[0] = R_alloc(8, sizeof(char));
    strcpy(args[0], "niimath");
    for (int i=1; i<nargs; i++)
    {
        const char *element = CHAR(STRING_ELT(_args, i-1));
        args[i] = R_alloc(strlen(element)+1, sizeof(char));
        strcpy(args[i], element);
    }
    
    const char *precision = CHAR(STRING_ELT(_precision, 0));
    if (!strcmp(precision, "single") || !strcmp(precision, "float"))
        status = main32(nargs, args);
    else
        status = main64(nargs, args);
    
    UNPROTECT(1);
    return ScalarInteger(status);
}

static R_CallMethodDef callMethods[] = {
    { "run",    (DL_FUNC) &run,     2 },
    { NULL, NULL, 0 }
};

void R_init_niimath (DllInfo *info)
{
   R_registerRoutines(info, NULL, callMethods, NULL, NULL);
   R_useDynamicSymbols(info, FALSE);
   R_forceSymbols(info, TRUE);
}
