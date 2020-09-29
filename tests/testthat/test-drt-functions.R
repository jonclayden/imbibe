testthat::context("Trying out 4D DRT functions")
fname = system.file("extdata/example_4d.nii.gz", package = "RNifti")
image = RNifti::readNifti(fname)
arr = array(image, dim = dim(image))
standard_func = function(func, arr) {
  RNifti::asNifti(apply(arr, 1:3, func), reference = image)
}

testthat::test_that("drt_mean", {
  std_mean = standard_func(mean, arr)
  equal_array(std_mean, run(drt_mean(image)))
})
