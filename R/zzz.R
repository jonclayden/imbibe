#' @import RNifti
#' @importFrom Rcpp evalCpp
#' @importFrom magrittr "%>%"
#' @useDynLib imbibe, .registration = TRUE, .fixes = "C_"
NULL

#' @export
magrittr::`%>%`

.onLoad <- function (libname, pkgname) {
    # Register tinytest extension if the package is installed
    if (system.file(package="tinytest") != "") {
        tinytest::register_tinytest_extension("imbibe", "expect_pipeline_result")
    }
}
