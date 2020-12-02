#ifndef _RNIFTI_SHIM_H_
#define _RNIFTI_SHIM_H_

#include <R.h>

#define RNIFTI_NIFTILIB_VERSION 2
#include "RNifti.h"

void setInputImages (nifti_image **images, const int n);

nifti_image * getInputImage (const char *name, const int keepData);

void setOutputImage (nifti_image *image);

nifti_image * getOutputImage ();

#endif
