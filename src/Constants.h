#ifndef OCL_KERNEL_BRANCH_COVERAGE_CHECKER_CONSTANTS
#define OCL_KERNEL_BRANCH_COVERAGE_CHECKER_CONSTANTS

namespace kernel_rewriter_constants{
    const char* const GLOBAL_COVERAGE_RECORDER_NAME = "ocl_kernel_branch_triggered_recorder";
    const char* const LOCAL_COVERAGE_RECORDER_NAME = "my_ocl_kernel_branch_triggered_recorder";
    const char* const FAKE_HEADER_MACRO = "OPENCLBC_FAKE_HEADER_FOR_LIBTOOLING_";
}

namespace error_code{
    const int TWO_MANY_HOST_FILE_SUPPLIED = 1;
    const int NO_HOST_FILE_SUPPLIED = 2;
}

#endif