#ifndef OPENCL_HOST_INVASTIGATOR
#define OPENCL_HOST_INVASTIGATOR

#include <string>
#include <map>

#include "clang/Tooling/Tooling.h"


std::map<int, std::string> invastigateHostFile(clang::tooling::ClangTool*);

#endif