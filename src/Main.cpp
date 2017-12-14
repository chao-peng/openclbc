#include <iostream>
#include <string>
#include <sstream>
#include <fstream>
#include <map>

#include "llvm/Support/CommandLine.h"
#include "clang/Tooling/CommonOptionsParser.h"

#include "HostCodeInvastigator.h"
#include "OpenCLKernelRewriter.h"
#include "Constants.h"
#include "UserConfig.h"

static llvm::cl::OptionCategory ToolCategory("OpenCL kernel branch coverage checker options");

static llvm::cl::opt<std::string> outputDirectory(
    "o",
    llvm::cl::desc("Specify the output directory"),
    llvm::cl::value_desc("directory"),
    llvm::cl::Required
);

static llvm::cl::opt<std::string> userConfigFileName(
    "config",
    llvm::cl::desc("Specify the user config file name"),
    llvm::cl::value_desc("filename"),
    llvm::cl::Optional // Will be empty string if not specified
);

int main(int argc, const char** argv){
    clang::tooling::CommonOptionsParser optionsParser(argc, argv, ToolCategory);

    auto it = optionsParser.getSourcePathList().begin();
    std::string kernelFileName(it->c_str());

    UserConfig userConfig(userConfigFileName.c_str());
    int numAddedLines = userConfig.generateFakeHeader(kernelFileName);

    clang::tooling::ClangTool tool(optionsParser.getCompilations(), optionsParser.getSourcePathList());

    std::string directory(outputDirectory.c_str());
    if (directory.at(directory.size() - 1) != '/') directory.append("/");
    rewriteOpenclKernel(&tool, directory, numAddedLines, &userConfig);

    UserConfig::removeFakeHeader(kernelFileName);
}