niimath <- function (image) {
    if (inherits(image, "niimath"))
        image
    else
        structure("#1", images=list(asNifti(image,internal=TRUE)), class="niimath")
}

.command <- function (init, flag, ...) {
    init <- niimath(init)
    elements <- as.character(c(init, flag))
    images <- attr(init, "images")
    args <- list(...)
    for (i in seq_along(args)) {
        if (is.numeric(args[[i]]))
            elements <- c(elements, paste(as.character(args[[i]]), collapse=" "))
        else {
            elements <- c(elements, paste0("#",length(images)+1))
            images <- c(images, list(asNifti(args[[i]],internal=TRUE)))
        }
    }
    structure(elements, images=images, class="niimath")
}

#' @export
run <- function (pipe, precision = c("double","float","single")) {
    precision <- match.arg(precision)
    .Call(C_run, pipe, precision)
}

#' Basic binary operations
#' 
#' @param image Image object or pipeline
#' @param arg Numeric or image argument
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

#' @export
threshold <- function (image, arg, frac = FALSE, nonzero = FALSE, above = FALSE) {
    if (!frac && !nonzero && !above)
        .command(image, "-thr", arg)
    else if (frac && !nonzero && !above)
        .command(image, "-thrp", arg)
    else if (frac && nonzero && !above)
        .command(image, "-thrP", arg)
    else if (!frac && !nonzero && above)
        .command(image, "-uthr", arg)
    else if (frac && !nonzero && above)
        .command(image, "-uthrp", arg)
    else if (frac && nonzero && above)
        .command(image, "-uthrP", arg)
    else
        stop("The specified set of options to threshold() is invalid")
}

#' Basic unary operations
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
binarise <- binarize <- function (image, invert = FALSE) .command(image, ifelse(invert,"-binv","-bin"))

#' Mathematical morphology kernels
#' 
#' @rdname kernels
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
#' @rdname morphology
#' @export dilate dilateall erode filter_median filter_mean smooth
dilate <- function (image, kernel = NULL, ..., max = FALSE, nonzero = TRUE) {
    flag <- ""
    if (!max && nonzero)
        flag <- "-dilM"
    else if (max && nonzero)
        flag <- "-dilD"
    else if (max && !nonzero)
        flag <- "-dilF"
    else
        stop("The specified set of options to dilate() is invalid")
    
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
smooth <- function (image, sigma)               .command(image, "-s", as.numeric(sigma))
subsample <- function (image, offset = FALSE)   .command(image, ifelse(offset,"-subsamp2offc","-subsamp2"))
