#include "core32.h"
#include "core64.h"
#include "RNifti_shim.h"
#include <Rinternals.h>

#ifdef _OPENMP
#include <omp.h>
#endif

SEXP run (SEXP _args, SEXP _precision, SEXP _threads)
{
    int status = 0;
    int nArgs;
    char **args;
    
    GetRNGstate();
    
    // Construct argument list: "-dt {float|double}" is required in practice
    PROTECT(_args = Rf_coerceVector(_args, STRSXP));
    nArgs = Rf_length(_args) + 3;
    args = (char **) R_alloc(nArgs, sizeof(char *));
    args[0] = R_alloc(8, sizeof(char));
    args[1] = R_alloc(4, sizeof(char));
    args[2] = R_alloc(8, sizeof(char));
    strcpy(args[0], "niimath");
    strcpy(args[1], "-dt");
    for (int i=3; i<nArgs; i++)
    {
        const char *element = CHAR(STRING_ELT(_args, i-3));
        args[i] = R_alloc(strlen(element)+1, sizeof(char));
        strcpy(args[i], element);
    }
    
    // If we have been passed images, retrieve them
    SEXP _images = Rf_getAttrib(_args, Rf_install("images"));
    if (Rf_isVectorList(_images))
    {
        const int nImages = Rf_length(_images);
        if (nImages > 0)
        {
            nifti_image **images = (nifti_image **) R_alloc(nImages, sizeof(nifti_image*));
            for (int i=0; i<nImages; i++)
                images[i] = (nifti_image *) R_ExternalPtrAddr(VECTOR_ELT(_images, i));
            setInputImages(images, nImages);
        }
    }
    
    // Set the number of threads, if appropriate
#ifdef _OPENMP
    const int threads = asInteger(_threads);
    omp_set_num_threads(threads == 0 ? omp_get_max_threads() : threads);
#endif
    
    const char *precision = CHAR(STRING_ELT(_precision, 0));
    if (!strcmp(precision, "single") || !strcmp(precision, "float"))
    {
        strcpy(args[2], "float");
        status = main32(nArgs, args);
    }
    else
    {
        strcpy(args[2], "double");
        status = main64(nArgs, args);
    }
    
    PutRNGstate();
    
    SEXP result = PROTECT(R_MakeExternalPtr(getOutputImage(), R_NilValue, R_NilValue));
    UNPROTECT(2);
    return result;
}

// R interface metadata
static R_CallMethodDef callMethods[] = {
    { "run",    (DL_FUNC) &run,     3 },
    { NULL, NULL, 0 }
};

// Package initialisation
void R_init_imbibe (DllInfo *info)
{
   R_registerRoutines(info, NULL, callMethods, NULL, NULL);
   R_useDynamicSymbols(info, FALSE);
   R_forceSymbols(info, TRUE);
   niftilib_register_all();
}
