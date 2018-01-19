#ifndef OPENCLBC_OPENCL_HOST_CODE_GENERATOR_H
#define OPENCLBC_OPENCL_HOST_CODE_GENERATOR_H

#include <sstream>
#include <string>
#include <map>
#include "UserConfig.h"

class HostCodeGenerator{
private:
    std::string kernelFunctionName;
    std::string branchRecorderArrayName;
    std::string barrierRecorderArrayName;
    std::string clContext;
    std::string errorCodeVariable;
    std::string clCommandQueue;
    int numConditions;
    int numBarriers;

    std::stringstream setArgumentPartHostCode;
    std::stringstream generatedHostCode;

public:
    HostCodeGenerator();

    void initialise(UserConfig* userConfig, int newNumConditions, int newNumBarriers);

    void setArgument(std::string functionName, int argumentLocation);

    void generateHostCode(std::string dataFilePath);

    std::string getGeneratedHostCode();
};

#endif