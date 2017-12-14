#ifndef OPENCLBC_OPENCL_KERNEL_REWRITER_H
#define OPENCLBC_OPENCL_KERNEL_REWRITER_H

#include <string>
#include <map>

#include "clang/Tooling/Tooling.h"
#include "UserConfig.h"


std::map<int, std::string> rewriteOpenclKernel(clang::tooling::ClangTool* tool, std::string newOutputFileName, int newNumAddedLines, UserConfig* userconfig);
#endif