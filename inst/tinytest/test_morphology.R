using("imbibe")

if (system.file(package="mmand") == "") {
    exit_file("Cannot check morphology operations without the \"mmand\" package")
}

image <- RNifti::readNifti(system.file("extdata", "example.nii.gz", package="RNifti"))
slice <- image[,,30]

box <- mmand::shapeKernel(c(3,3), type="box")
disc <- mmand::shapeKernel(c(3,3), type="disc")

expect_pipeline_result(slice %>% kernel_2d() %>% dilate(max=TRUE, nonzero=FALSE), mmand::dilate(slice, box))
expect_pipeline_result(slice %>% dilate(kernel=kernel_sphere, 1.5, max=TRUE, nonzero=FALSE), mmand::dilate(slice, disc))
expect_pipeline_result(slice %>% kernel_2d() %>% erode(min=TRUE), mmand::erode(slice, box))
expect_pipeline_result(slice %>% filter_mean(kernel=kernel_2d), mmand::meanFilter(slice, box), tolerance=1e-6)
expect_pipeline_result(slice %>% filter_median(kernel=kernel_2d), mmand::medianFilter(slice, box))
