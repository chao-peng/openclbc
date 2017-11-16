#ifndef OPENCL_KERNEL_REWRITER
#define OPENCL_KERNEL_REWRITER

#include <string>
#include <map>

#include "clang/Tooling/Tooling.h"

std::map<int, std::string> rewriteOpenclKernel(clang::tooling::ClangTool* tool, std::string newOutputFileName);
#endif