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
#' @param precision The internal precision used for calculations. \code{"float"} and
#'   \code{"single"} are equivalent.
#' @return An image
#' 
#' @export
run <- function (pipe, precision = c("double","float","single")) {
    precision <- match.arg(precision)
    .Call(C_run, pipe, precision)
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
