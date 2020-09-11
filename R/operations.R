niimath <- function (image) {
    if (inherits(image, "niimath"))
        image
    else
        structure("#1", images=list(asNifti(image,internal=TRUE)), class="niimath")
}

.command <- function (init, flag, ...) {
    elements <- niimath(init)
    images <- attr(elements, "images")
    args <- list(...)
    for (i in seq_along(args)) {
        if (is.numeric(args[[i]]))
            elements <- c(elements, as.character(args[[i]]))
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

#' Binary operations
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
        stop("The specified set of options to threshold() are invalid")
}
