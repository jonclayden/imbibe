#include "core32.h"
#include "core64.h"
#include "RNifti_shim.h"

#ifdef _OPENMP
#include <omp.h>
#endif

RcppExport SEXP run (SEXP _args, SEXP _precision, SEXP _threads)
{
BEGIN_RCPP
    int status = 0;
    Rcpp::RNGScope scope;
    
    // Construct argument list: "-dt {float|double}" is required in practice
    Rcpp::CharacterVector argsR(_args);
    const int nArgs = argsR.length() + 3;
    char **args = (char **) R_alloc(nArgs, sizeof(char *));
    args[0] = R_alloc(8, sizeof(char));
    args[1] = R_alloc(4, sizeof(char));
    args[2] = R_alloc(8, sizeof(char));
    strcpy(args[0], "niimath");
    strcpy(args[1], "-dt");
    for (int i=3; i<nArgs; i++)
    {
        const char *element = argsR[i-3];
        args[i] = R_alloc(strlen(element)+1, sizeof(char));
        strcpy(args[i], element);
    }
    
    // If we have been passed images, retrieve them
    if (argsR.hasAttribute("images"))
    {
        Rcpp::List imageList(argsR.attr("images"));
        int nImages = imageList.length();
        if (nImages > 0)
        {
            nifti_image **images = (nifti_image **) R_alloc(nImages, sizeof(nifti_image*));
            for (int i=0; i<nImages; i++)
            {
                // Each nifti_image cannot be owned by its NiftiImage, because
                // niimath handles deallocation, so we have to copy it manually
                SEXP imagePtr = imageList[i];
                RNifti::NiftiImage image(imagePtr, true, true);
                images[i] = nifti_copy_nim_info(image);
                if (!image.data().isEmpty())
                {
                    size_t dataSize = nifti_get_volsize(image);
                    images[i]->data = calloc(1, dataSize);
                    memcpy(images[i]->data, image->data, dataSize);
                }
            }
            setInputImages(images, nImages);
        }
    }
    
    // Set the number of threads, if appropriate
#ifdef _OPENMP
    const int threads = Rcpp::as<int>(_threads);
    omp_set_num_threads(threads == 0 ? omp_get_max_threads() : threads);
#endif
    
    // Add the precision argument and run the pipeline
    const std::string precision = Rcpp::as<std::string>(_precision);
    if (precision == "single" || precision == "float")
    {
        strcpy(args[2], "float");
        status = main32(nArgs, args);
    }
    else
    {
        strcpy(args[2], "double");
        status = main64(nArgs, args);
    }
    
    // Capture the output and return
    nifti_image *outputImage = getOutputImage();
    RNifti::NiftiImage result(outputImage);
    return result.toPointer("NIfTI image");
END_RCPP
}

// C linkage for R
extern "C" {

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

} // extern "C"
