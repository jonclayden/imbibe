library(testthat)
library(imbibe)

equal_array = function(object, expected, ...) {
  expected = as.array(expected)
  object = as.array(object)
  testthat::expect_equal(array(object, dim = dim(object)),
                         array(expected, dim = dim(expected)),
                         ...)
}

test_check("imbibe")
