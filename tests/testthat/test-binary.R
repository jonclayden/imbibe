testthat::context("Trying out Binary and one-type operations")
fname = system.file("extdata/example.nii.gz", package = "RNifti")
image = RNifti::readNifti(fname)
arr = array(image, dim = dim(image))

value = 1000
testthat::test_that("standard binary operations", {
  equal_array(image + value, add(image, value) %>% run())
  equal_array(image - value, subtract(image, value) %>% run())
  equal_array(image * value, multiply(image, value) %>% run())
  equal_array(image / value, divide(image, value) %>% run(), tolerance = 1e-7)
  equal_array(image / value, divide(image, value) %>% run(), tolerance = 1e-7)
})


value = 1000
testthat::test_that("min/max", {
  equal_array(
    array(pmin(c(arr), 1), dim = dim(arr)),
    minimum(image, 1) %>% run())
  equal_array(
    array(pmax(c(arr), 140), dim = dim(arr)),
    maximum(image, 140) %>% run())
  equal_array(
    arr %% 2,
    remainder(image, 2) %>% run())
  equal_array(
    arr %% 140,
    remainder(image, 140) %>% run())
})
