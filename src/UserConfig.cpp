#include <set>
#include <string>
#include <sstream>
#include <fstream>
#include <iostream>

#include "UserConfig.h"
#include "Constants.h"

int UserConfig::generateFakeHeader(std::string configFileName, std::string kernelFileName){
    int numAddedLines = 0;
    if (hasFakeHeader(kernelFileName)) {
        return 0;
    }
    std::set<std::string> setMacro;
    std::fstream configFileStream(configFileName, std::ios::in);
    std::string line;
    while (std::getline(configFileStream, line)){
        if(line == "[MACRO]") {
            while(std::getline(configFileStream,line)){
                if (line == "[ENDMACRO]") break;
                setMacro.insert(line);
            }
        }
    }
    
    std::stringstream header;
    header << "#ifndef " << kernel_rewriter_constants::FAKE_HEADER_MACRO << "\n";
    header << "#define " << kernel_rewriter_constants::FAKE_HEADER_MACRO << "\n";
    header << "#include <opencl-c.h>\n"; // for opencl library calls
    numAddedLines += 3;
    for (auto it = setMacro.begin(); it != setMacro.end(); it++) {
        header << "#define " << *it << "\n";
        numAddedLines++;
    }
    header << "#endif\n";
    numAddedLines++;

    std::stringstream kernelSource;
    std::fstream kernelFileStream(kernelFileName, std::ios::in);
    kernelSource << header.str();

    while(std::getline(kernelFileStream, line)){
        kernelSource<<line<<"\n";
    }
    kernelFileStream.close();

    std::fstream newKernelFileStream(kernelFileName, std::ios::out);
    newKernelFileStream << kernelSource.str();
    newKernelFileStream.close();

    return numAddedLines;
}

int UserConfig::removeFakeHeader(std::string kernelFileName){
    if (!hasFakeHeader(kernelFileName)){
        return error_code::REMOVE_KERNEL_FAKE_HEADER_FAILED_KERNEL_DOES_NOT_EXIST;
    }

    std::ifstream fileReader;
    std::ofstream fileWriter;
    std::string recoveredKernelCode;
    std::string line;
    fileReader.open(kernelFileName);

    std::string targetLine = "#ifndef ";
    targetLine.append(kernel_rewriter_constants::FAKE_HEADER_MACRO);
    std::string targetLine2 = "#endif";

    while(std::getline(fileReader, line)){
        if (line != targetLine){
            recoveredKernelCode.append(line);
            recoveredKernelCode.append("\n");
        } else {
            while(std::getline(fileReader, line)){
                if (line == targetLine2) {
                    break;
                }
            }
        }
    }
    fileReader.close();
    fileWriter.open(kernelFileName);
    fileWriter << recoveredKernelCode;
    fileWriter.close();
    return error_code::STATUS_OK;
}

bool UserConfig::hasFakeHeader(std::string kernelFileName){
    std::ifstream kernelFileStream(kernelFileName);
    std::string line;
    std::string targetLine = "#ifndef ";
    targetLine.append(kernel_rewriter_constants::FAKE_HEADER_MACRO);
    while (std::getline(kernelFileStream, line)){
        if (line.find(targetLine) != std::string::npos) {
            return true;
        }
    }
    return false;
}