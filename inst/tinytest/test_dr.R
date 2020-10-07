using("imbibe")

image <- RNifti::readNifti(system.file("extdata", "example.nii.gz", package="RNifti"))

# Medians don't match because even dims are not resolved the same way
functions <- list(dim_mean = mean,
                  dim_sd = sd,
                  dim_max = max,
                  dim_whichmax = which.max,
                  dim_min = min)

for (i in seq_along(functions)) {
    ifun <- get(names(functions)[i], "package:imbibe")
    rfun <- match.fun(functions[[i]])
    expect_pipeline_result(ifun(image,dim=1L), apply(image,2:3,rfun), info=paste("Function is", names(functions)[i]), tolerance=1e-6, check.attributes=FALSE)
    expect_pipeline_result(ifun(image,dim=1L), apply(image,2:3,rfun), info=paste("Function is", names(functions)[i]), precision="single", tolerance=1e-6, check.attributes=FALSE)
}
