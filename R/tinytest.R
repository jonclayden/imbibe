.attribfilter <- function (x) {
    for (hidden in grep("^\\.", names(attributes(x)), value=TRUE))
        attr(x, hidden) <- NULL
    return (x)
}

#' Expectation for testing pipeline output
#' 
#' This function provides an expectation for use with the "tinytest" package,
#' which runs the pipeline specified in its first argument and compares the
#' result to its second.
#' 
#' @param current The pipeline to run, which should have class \code{"imbibe"}.
#' @param target The target value to compare against, a numeric array of some
#'   kind, which will be converted to a \code{"niftiImage"} object.
#' @param precision A string specifying the working precision. Passed to
#'   \code{\link{run}}.
#' @param ... Further arguments to \code{expect_equal}.
#' @return A \code{"tinytest"} object.
#' 
#' @rdname tinytest
#' @export
expect_pipeline_result <- function (current, target, precision = "double", ...) {
    if (!inherits(current, "imbibe"))
        stop("Pipeline under test does not appear to be valid")
    call <- sys.call(sys.parent(1))
    current <- .attribfilter(as.array(run(current, precision=precision)))
    target <- .attribfilter(as.array(asNifti(target)))
    result <- tinytest::expect_equal(current, target, ...)
    return (structure(result, call=call))
}
