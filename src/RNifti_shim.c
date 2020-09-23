#include "RNifti_shim.h"
#include "RNiftiAPI.h"

static nifti_image **inputImages = NULL;
static int nInputImages = 0;
static nifti_image *outputImage = NULL;

void setInputImages (nifti_image **images, const int n)
{
    inputImages = images;
    nInputImages = n;
}

nifti_image * getInputImage (const char *name, const int keepData)
{
    if (name[0] != '#')
        Rf_error("Image placeholder \"%s\" is not valid", name);
    else if (inputImages == NULL || nInputImages == 0)
        Rf_error("No images are available");
    
    const int i = atoi(name+1) - 1;
    if (i < 0 || i >= nInputImages)
        Rf_error("Image placeholder value \"%s\" is out of bounds", name);
    
    return inputImages[i];
}

void setOutputImage (nifti_image *image)
{
    outputImage = nifti_copy_nim_info(image);
    if (image->data != NULL)
    {
        size_t dataSize = nifti_get_volsize(image);
        outputImage->data = calloc(1, dataSize);
        memcpy(outputImage->data, image->data, dataSize);
    }
}

nifti_image * getOutputImage ()
{
    return outputImage;
}
