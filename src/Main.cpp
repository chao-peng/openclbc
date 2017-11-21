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

static llvm::cl::OptionCategory ToolCategory("OpenCL kernel branch coverage checker options");

static llvm::cl::opt<std::string> outputDirectory(
    "o",
    llvm::cl::desc("Specify the output directory"),
    llvm::cl::value_desc("directory"),
    llvm::cl::Required
);

int main(int argc, const char** argv){
    clang::tooling::CommonOptionsParser optionsParser(argc, argv, ToolCategory);
    clang::tooling::ClangTool tool(optionsParser.getCompilations(), optionsParser.getSourcePathList());
    
    /*
    std::string *inputHostFile = NULL;
    std::vector<std::string> kernelFiles;
    std::map<int, std::string> inputKernelFiles;
    //iterate over file list
    for (auto it = optionsParser.getSourcePathList().begin() ; it != optionsParser.getSourcePathList().end(); ++it){
        if (it->substr(it->find_last_of(".")+1) == "cpp") {
            if (inputHostFile) {
                std::cout << "Error: Too many host file supplied." << std::endl;
                exit(error_code::TWO_MANY_HOST_FILE_SUPPLIED);
            } else {
                inputHostFile = new std::string(*it);
                std::cout << "Host source code [" << *inputHostFile << "] added" << std::endl;
            }
        }
    }

    if (!inputHostFile) {
        std::cout << "Error: Not host source code supplied" << std::endl;
        exit(error_code::NO_HOST_FILE_SUPPLIED);
    }

    inputKernelFiles = invastigateHostFile(&tool);
*/

    std::map<int, std::string> branchMap;
    std::string directory(outputDirectory.c_str());
    if (directory.at(directory.size() - 1) != '/') directory.append("/");
    std::string dataFileAddr(directory);
    dataFileAddr.append("ocl_bc.dat");
    branchMap = rewriteOpenclKernel(&tool, directory);

    /*
    std::ofstream fileWriter;
    std::stringstream outputBuffer;
    fileWriter.open(dataFileAddr);
    for (auto it = branchMap.begin(); it != branchMap.end(); it++){
        //outputBuffer << it->first << "\n";
        outputBuffer << it->second << "\n";
    }
    outputBuffer << "\n";
    fileWriter << outputBuffer.str();
    fileWriter.close();
    
    std::cout << "Number of conditions found: " << branchMap.size() << std::endl;
    std::cout << "Branch data has been written to ocl.bc.dat" << std::endl;
    */
    /*
    for (auto it = optionsParser.getSourcePathList().begin() ; it != optionsParser.getSourcePathList().end(); ++it)
        std::cout << *it << '\n';
    return 0;
    */
}