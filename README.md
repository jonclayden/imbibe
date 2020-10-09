

<!-- badges: start -->
[![Build Status](https://travis-ci.org/jonclayden/imbibe.svg?branch=master)](https://travis-ci.org/jonclayden/imbibe)
<!-- badges: end -->

# imbibe: A pipe-friendly image calculator <img src='tools/figures/logo-small.png' align="right" />

The `imbibe` [R package](https://www.r-project.org) offers fast, chainable image-processing operations which are applicable to images of two, three or four dimensions. It provides an R interface to the core C functions of the [`niimath` project](https://github.com/rordenlab/niimath) by Chris Rorden, which is in turn a free-software reimplementation of [`fslmaths`](https://fsl.fmrib.ox.ac.uk/fsl/fslwiki/Fslutils), so it has its roots in medical image analysis and is particularly well-suited to such data. The package was designed from the outset to work well with the [pipe syntax](https://github.com/tidyverse/magrittr) widely popularised amongst R users by the [Tidyverse](https://www.tidyverse.org/) family of packages.

## Installation

This package is still at quite an early stage of development. The latest version can be easily installed using the `remotes` package.


```r
## install.packages("remotes")
remotes::install_github("jonclayden/imbibe")
```

## Usage

The first step for any usage of the package is to create or read in an image. We will use a 3D medical image from the [`RNifti` package](https://github.com/jonclayden/RNifti) by way of an example.


```r
library(RNifti)
library(imbibe)
image <- readNifti(system.file("extdata", "example.nii.gz", package="RNifti"))
```

We can also use the `RNifti` image viewer to visualise the image.


```r
view(image)
```

![plot of chunk original](tools/figures/original-1.png)

A simple example operation would be to smooth the image with a Gaussian smoothing kernel of standard deviation 4 mm. We can use standard R syntax to perform this operation, return a result, and then show it:


```r
smoothed <- run(smooth_gauss(image, 4))
view(smoothed)
## Setting window to (0, 549.9)
```

![plot of chunk standard](tools/figures/standard-1.png)

Here, `smooth_gauss()` requests the smoothing operation, and `run()` actually runs the pipeline and returns the processed image.

However, the pipe syntax provides an alternative, which can be further simplified because calling `view()` on a pipeline will implicitly run it.


```r
image %>% smooth_gauss(4) %>% view()
## Setting window to (0, 549.9)
```

![plot of chunk pipe](tools/figures/pipe-1.png)

Notice now `smooth_gauss()` is now called with only one argument, and `view()` with none, because the input to the pipe (`%>%`) is implicitly added first. The benefits to readability of this approach increase substantially as more operations are added to the chain:


```r
image %>% kernel_sphere(radius=3) %>% dilate() %>% subtract(image) %>% view()
## Setting window to (0, 60)
```

![plot of chunk dilate](tools/figures/dilate-1.png)

This example sets up a spherical [kernel](https://en.wikipedia.org/wiki/Kernel_(image_processing)) of radius 3 mm, dilates the image with it, and then subtracts the original image from the result to leave just the outer edge of the imaged object.
