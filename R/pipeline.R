#' Create an operation pipeline
#' 
#' @param image An image object or existing pipeline.
#' @param x An \code{"imbibe"} object.
#' @param ... Additional arguments to methods.
#' 
#' @export
imbibe <- function (image) {
    if (inherits(image, "imbibe"))
        image
    else
        structure("#1", images=list(asNifti(image,internal=TRUE)), class="imbibe")
}

.command <- function (init, flag, ...) {
    init <- imbibe(init)
    elements <- as.character(c(init, flag))
    images <- attr(init, "images")
    args <- list(...)
    for (i in seq_along(args)) {
        if (is.numeric(args[[i]]) && is.null(dim(args[[i]])))
            elements <- c(elements, paste(as.character(args[[i]]), collapse=" "))
        else {
            elements <- c(elements, paste0("#",length(images)+1))
            images <- c(images, list(asNifti(args[[i]],internal=TRUE)))
        }
    }
    structure(elements, images=images, class="imbibe")
}


#' Run a pipeline and return an image result
#' 
#' @param pipe An operation pipeline.
#' @param precision The internal precision used for calculations. May be
#'   \code{"double"}, \code{"float"} or \code{"single"}; the latter two are
#'   equivalent.
#' @return An image
#' 
#' @examples
#' im <- RNifti::readNifti(system.file("extdata", "example.nii.gz", package="RNifti"))
#' pipe <- im %>% threshold_below(500) %>% binarise()
#' run(pipe)
#' @export
run <- function (pipe, precision = getOption("imbibe.precision","double")) {
    precision <- match.arg(precision, c("double","float","single"))
    .Call(C_run, pipe, precision, 0L)
}


#' @rdname imbibe
#' @export
asNifti.imbibe <- function (x, ...) {
    asNifti(run(x), ...)
}


#' @rdname imbibe
#' @export
as.array.imbibe <- function (x, ...) {
    as.array(run(x))
}


#' @rdname imbibe
#' @export
print.imbibe <- function (x, ...) {
    print(structure(paste(x, collapse=" "), images=attr(x,"images")))
}
