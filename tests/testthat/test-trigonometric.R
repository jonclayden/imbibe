testthat::context("Trying out Trig functions")
fname = system.file("extdata/example.nii.gz", package = "RNifti")
image = RNifti::readNifti(fname)


sqr = function(x) {
  x ^ 2
}

functions = c(exponent = "exp",
              logarithm = "log",
              sine = "sin",
              cosine = "cos",
              tangent = "tan",
              arcsine = "asin",
              arccosine = "acos",
              arctangent = "atan",
              square = "sqr",
              squareroot = "sqrt",
              reciprocal = "recip",
              absolute = "abs")



testthat::test_that("trig works", {
  results = mapply(function(x, y) {
    base_func = get(x)
    im_func = get(y)
    print(paste0("function is ", y))
    im_out =  run(im_func(image))
    base_out = run(base_func(image))
    equal_array(im_out, base_out)
    list(im_out = im_out,
         base_out = base_out)
  }, functions, names(functions))
})
