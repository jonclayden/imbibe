using("imbibe")

image <- RNifti::readNifti(system.file("extdata", "example_4d.nii.gz", package="RNifti"))


functions <- list(drt_mean = mean,
                  drt_sd = sd,
                  drt_max = max,
                  drt_whichmax = which.max,
                  drt_min = min,
                  drt_median = median)

for (i in seq_along(functions)) {
    ifun <- get(names(functions)[i], "package:imbibe")
    rfun <- match.fun(functions[[i]])
    expect_pipeline_result(ifun(image), RNifti::asNifti(apply(image,1:3,rfun), image), info=paste("Function is", names(functions)[i]), tolerance=1e-6)
}
