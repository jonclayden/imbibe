#' Basic binary operations
#'
#' @param image An image object or pipeline.
#' @param arg Numeric or image argument.
#' @return An updated pipeline.
#'
#' @rdname binary
#' @export add subtract multiply divide remainder mask maximum minimum
add <- function (image, arg)        .command(image, "-add", arg)
#' @rdname binary
subtract <- function (image, arg)   .command(image, "-sub", arg)
#' @rdname binary
multiply <- function (image, arg)   .command(image, "-mul", arg)
#' @rdname binary
divide <- function (image, arg)     .command(image, "-div", arg)
#' @rdname binary
remainder <- function (image, arg)  .command(image, "-rem", arg)
#' @rdname binary
mask <- function (image, arg)       .command(image, "-mas", arg)
#' @rdname binary
maximum <- function (image, arg)    .command(image, "-max", arg)
#' @rdname binary
minimum <- function (image, arg)    .command(image, "-min", arg)


#' Image thresholding
#'
#' @param image An image object or pipeline.
#' @param value Numeric threshold value.
#' @param reference String indicating what the \code{value} should be
#'   referenced against, if anything. If \code{"none"}, the default, the
#'   \code{value} is taken literally. If \code{"image"}, it is interpreted as
#'   a proportion of the "robust range" of the current image's intensities. If
#'   \code{"nonzero"} it is interpreted as a proportion of the "robust range"
#'   of the nonzero pixel intensities.
#' @param above Logical value: if \code{TRUE} the operation zeroes values above
#'   the threshold; otherwise it zeroes values below it. The
#'   \code{threshold_below} and \code{threshold_above} function variants set
#'   argument implicitly.
#' @return An updated pipeline.
#'
#' @export threshold threshold_below threshold_above
threshold <- function (image, value, reference = c("none","image","nonzero"), above = FALSE) {
    reference <- match.arg(reference)
    if (above)
        .command(image, switch(reference,none="-uthr",image="-uthrp",nonzero="-uthrP"), value)
    else
        .command(image, switch(reference,none="-thr",image="-thrp",nonzero="-thrP"), value)
}

#' @rdname threshold
threshold_below <- function (image, value, reference = c("none","image","nonzero")) {
    threshold(image, value, reference, above=FALSE)
}

#' @rdname threshold
threshold_above <- function (image, value, reference = c("none","image","nonzero")) {
    threshold(image, value, reference, above=TRUE)
}


#' Basic unary operations
#'
#' @param image An image object or pipeline.
#' @param invert Logical value: if \code{TRUE}, binarising will also perform
#'   logical inversion so that only zeroes in the original image will be
#'   nonzero; if \code{FALSE}, the default, the usual sense is used, in which
#'   zeroes remain as they are, and everything else is converted to 1.
#' @return An updated pipeline.
#'
#' @rdname unary
#' @export exponent logarithm sine cosine tangent arcsine arccosine arctangent square squareroot reciprocal absolute binarise binarize
exponent <- function (image)    .command(image, "-exp")
#' @rdname unary
logarithm <- function (image)   .command(image, "-log")
#' @rdname unary
sine <- function (image)        .command(image, "-sin")
#' @rdname unary
cosine <- function (image)      .command(image, "-cos")
#' @rdname unary
tangent <- function (image)     .command(image, "-tan")
#' @rdname unary
arcsine <- function (image)     .command(image, "-asin")
#' @rdname unary
arccosine <- function (image)   .command(image, "-acos")
#' @rdname unary
arctangent <- function (image)  .command(image, "-atan")
#' @rdname unary
square <- function (image)      .command(image, "-sqr")
#' @rdname unary
squareroot <- function (image)  .command(image, "-sqrt")
#' @rdname unary
reciprocal <- function (image)  .command(image, "-recip")
#' @rdname unary
absolute <- function (image)    .command(image, "-abs")
#' @rdname unary
binarise <- function (image, invert = FALSE) .command(image, ifelse(invert,"-binv","-bin"))
#' @rdname unary
binarize <- binarise


#' Mathematical morphology kernels
#'
#' @param image An image object or pipeline.
#' @param width The width of the kernel in appropriate units. If \code{voxels}
#'   is \code{FALSE} a value can be specified for each of the three dimensions;
#'   otherwise only a single value should be given and the kernel will be
#'   isotropic.
#' @param voxels Logical value: if \code{TRUE}, the \code{width} is given in
#'   pixels/voxels and must be an odd integer; otherwise, the units are
#'   millimetres and can take any value.
#' @param sigma Numeric value giving the standard deviation of a Gaussian
#'   kernel, in millimetres.
#' @param radius Numeric value giving the radius of a sphere kernel, in
#'   millimetres.
#' @param file Name of a NIfTI file containing the kernel.
#' @return An updated pipeline.
#'
#' @rdname kernels
#' @aliases kernels
#' @export kernel_3d kernel_2d kernel_box kernel_gauss kernel_sphere kernel_file
kernel_3d <- function (image)   .command(image, c("-kernel","3D"))
#' @rdname kernels
kernel_2d <- function (image)   .command(image, c("-kernel","2D"))
#' @rdname kernels
kernel_box <- function (image, width, voxels = FALSE) {
    if (length(width) == 1) {
        if (voxels)
            .command(image, c("-kernel","boxv"), as.integer(width))
        else
            .command(image, c("-kernel","box"), as.numeric(width))
    } else if (!voxels && length(width) == 3)
        .command(image, c("-kernel","boxv3"), as.integer(width))
    else
        stop("The specified set of options to kernel_box() is invalid")
}
#' @rdname kernels
kernel_gauss <- function (image, sigma)     .command(image, c("-kernel","gauss"), as.numeric(sigma))
#' @rdname kernels
kernel_sphere <- function (image, radius)   .command(image, c("-kernel","sphere"), as.numeric(radius))
#' @rdname kernels
kernel_file <- function (image, file)       .command(image, c("-kernel","file",file))


#' Mathematical morphology and filtering operations
#'
#' @param image An image object or pipeline.
#' @param kernel A suitable kernel function (see \code{\link{kernels}}). If
#'   \code{NULL}, the most recently set kernel in the pipeline is used, if any,
#'   otherwise the default kernel (\code{kernel_3d}).
#' @param ... Additional arguments to the kernel function, if any.
#' @param max Logical value: if \code{TRUE}, maximum filtering is used for
#'   dilation; otherwise mean filtering is used. Mean filtering is always used
#'   by \code{dilateall}.
#' @param nonzero Logical value: if \code{TRUE}, the default, dilation is only
#'   applied to nonzero pixels/voxels. Otherwise it is applied everywhere (and
#'   maximum filtering is always used).
#' @param min Logical value: if \code{TRUE}, minimum filtering is used for
#'   erosion; otherwise nonzero voxels overlapping with the kernel are simply
#'   zeroed.
#' @param norm Logical value indicating whether the mean filter will be
#'   normalised or not.
#' @param sigma Numeric value giving the standard deviation of the Gaussian
#'   smoothing kernel.
#' @param offset Logical value indicating whether subsampled pixels should be
#'   offset from the original locations or not.
#' @return An updated pipeline.
#'
#' @rdname morphology
#' @export dilate dilateall erode filter_median filter_mean smooth_gauss
dilate <- function (image, kernel = NULL, ..., max = FALSE, nonzero = TRUE) {
    flag <- ""
    if (!max && nonzero)
        flag <- "-dilM"
    else if (max && nonzero)
        flag <- "-dilD"
    else if (max && !nonzero)
        flag <- "-dilF"
    else {
        flag <- "-dilF"
        warning("Maximum filtering is always used when including zero voxels")
    }

    if (is.null(kernel))
        .command(image, flag)
    else
        .command(kernel(image, ...), flag)
}

#' @rdname morphology
dilateall <- function (image, kernel = NULL, ...) {
    if (is.null(kernel))
        .command(image, "-dilall")
    else
        .command(kernel(image, ...), "-dilall")
}

#' @rdname morphology
erode <- function (image, kernel = NULL, ..., min = FALSE) {
    flag <- ifelse(min, "-eroF", "-ero")
    if (is.null(kernel))
        .command(image, flag)
    else
        .command(kernel(image, ...), flag)
}

#' @rdname morphology
filter_median <- function (image, kernel = NULL, ...) {
    if (is.null(kernel))
        .command(image, "-fmedian")
    else
        .command(kernel(image, ...), "-fmedian")
}

#' @rdname morphology
filter_mean <- function (image, kernel = NULL, ..., norm = TRUE) {
    flag <- ifelse(norm, "-fmean", "-fmeanu")
    if (is.null(kernel))
        .command(image, flag)
    else
        .command(kernel(image, ...), flag)
}

#' @rdname morphology
smooth_gauss <- function (image, sigma)         .command(image, "-s", as.numeric(sigma))

#' @rdname morphology
subsample <- function (image, offset = FALSE)   .command(image, ifelse(offset,"-subsamp2offc","-subsamp2"))


.reduce <- function (image, dim, op, ...) {
    if (!is.numeric(dim) || dim < 1 || dim > 4)
        stop("The specified dimension is out of bounds")
    dimname <- c("X","Y","Z","T")[dim]
    .command(image, paste0("-", dimname, op), ...)
}

#' Dimensionality reduction operations
#'
#' @param image An image object or pipeline.
#' @param dim Integer value between 1 and 4, giving the dimension to apply the
#'   reduction along.
#' @param prob For \code{drt_quantile}, the quantile probability to extract
#'   (analogously to \code{\link{quantile}}).
#' @return An updated pipeline.
#'
#' @rdname reduce
#' @export dim_mean dim_sd dim_max dim_whichmax dim_min dim_median dim_quantile dim_AR1
dim_mean <- function (image, dim = 4L)              .reduce(image, dim, "mean")
#' @rdname reduce
dim_sd <- function (image, dim = 4L)                .reduce(image, dim, "std")
#' @rdname reduce
dim_max <- function (image, dim = 4L)               .reduce(image, dim, "max")
#' @rdname reduce
dim_whichmax <- function (image, dim = 4L)          .command(.reduce(image,dim,"maxn"), "-add", 1)
#' @rdname reduce
dim_min <- function (image, dim = 4L)               .reduce(image, dim, "min")
#' @rdname reduce
dim_median <- function (image, dim = 4L)            .reduce(image, dim, "median")
#' @rdname reduce
dim_quantile <- function (image, dim = 4L, prob)    .reduce(image, dim, "perc", prob*100)
#' @rdname reduce
dim_AR1 <- function (image, dim = 4L)               .reduce(image, dim, "ar1")
