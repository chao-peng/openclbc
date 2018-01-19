#ifndef OPENCLBC_OPENCL_KERNEL_REWRITER_H
#define OPENCLBC_OPENCL_KERNEL_REWRITER_H

#include <string>
#include <map>

#include "clang/Tooling/Tooling.h"
#include "UserConfig.h"


int rewriteOpenclKernel(clang::tooling::ClangTool* tool, std::string newOutputFileName, UserConfig* userconfig);
#endif