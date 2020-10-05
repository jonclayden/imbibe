using("imbibe")

slice <- RNifti::readNifti(system.file("extdata", "example.nii.gz", package="RNifti"))[,,30]

expect_pipeline_result(slice %>% threshold_below(300), ifelse(slice < 300, 0, slice))
expect_pipeline_result(slice %>% threshold_above(300), ifelse(slice > 300, 0, slice))

slicemask <- slice %>% threshold_below(500) %>% binarise() %>% run()
expect_equal(as.array(slicemask), RNifti::asNifti(ifelse(slice < 500, 0, 1)), check.attributes=FALSE)
expect_pipeline_result(slice %>% multiply(-1) %>% mask(slicemask), ifelse(slice < 500, 0, -slice))
