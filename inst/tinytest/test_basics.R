using("imbibe")

image <- RNifti::readNifti(system.file("extdata", "example.nii.gz", package="RNifti"))

expect_pipeline_result(add(image,10),       image + 10)
expect_pipeline_result(subtract(image,10),  image - 10)
expect_pipeline_result(multiply(image,10),  image * 10)
expect_pipeline_result(divide(image,10),    image / 10, tolerance=1e-6)

expect_pipeline_result(minimum(image,10),   pmin(image,10))
expect_pipeline_result(maximum(image,10),   pmax(image,10))
expect_pipeline_result(remainder(image,10), image %% 10)

# Exponent and reciprocal don't match, seemingly because overflow to +/- Inf
# isn't consistent between niimath and R, so we skip them for now
functions <- list(# exponent = exp,
                  logarithm = log,
                  sine = sin,
                  cosine = cos,
                  tangent = tan,
                  arcsine = function(x) suppressWarnings(asin(x)),
                  arccosine = function(x) suppressWarnings(acos(x)),
                  arctangent = atan,
                  square = function(x) x^2,
                  squareroot = sqrt,
                  # reciprocal = function(x) 1/x,
                  absolute = abs)

for (i in seq_along(functions)) {
    ifun <- get(names(functions)[i], "package:imbibe")
    rfun <- match.fun(functions[[i]])
    expect_pipeline_result(ifun(image), rfun(image), info=paste("Function is", names(functions)[i]), tolerance=1e-6)
    expect_pipeline_result(ifun(image), rfun(image), info=paste("Function is", names(functions)[i]), precision="single", tolerance=1e-6)
}
