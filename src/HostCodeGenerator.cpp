#include <sstream>
#include <string>
#include <map>
#include <iostream>

#include "HostCodeGenerator.h"
#include "Constants.h"
#include "UserConfig.h"

HostCodeGenerator::HostCodeGenerator(){
    setArgumentPartHostCode << "Part 2 - set argument to kernel function\n";
}

void HostCodeGenerator::initialise(UserConfig* userConfig, int newNumConditions, int newNumBarriers){
    kernelFunctionName = userConfig->getValue("kernel_function_name");
    branchRecorderArrayName = kernelFunctionName + "_branch_coverage_recorder";
    barrierRecorderArrayName = kernelFunctionName + "_barrier_divergence_recorder";
    clContext = userConfig->getValue("cl_context");
    errorCodeVariable = userConfig->getValue("error_code_variable");
    clCommandQueue = userConfig->getValue("cl_command_queue");
    numConditions = newNumConditions;
    numBarriers = newNumBarriers;
}

void HostCodeGenerator::setArgument(std::string functionName, int argumentLocation){
    if(numConditions){
        setArgumentPartHostCode 
            << errorCodeVariable << " = clSetKernelArg(" << functionName << ", " << argumentLocation++ << ", sizeof(cl_mem), &d_" << branchRecorderArrayName << ");\n";
    }
    if(numBarriers){
        setArgumentPartHostCode 
            << errorCodeVariable << " = clSetKernelArg(" << functionName << ", " << argumentLocation << ", sizeof(cl_mem), &d_" << barrierRecorderArrayName << ");\n";
    }
}

void HostCodeGenerator::generateHostCode(std::string dataFilePath){
    // Introduction
    generatedHostCode << "Generated host code as following can be used as a guide line to "
        << "initialise and manage data elements for checking code coverage and print out the "
        << "coverage report. Please use it as a reference \n\n";

    // Host code part 1 - declare branch coverage recorder array
    generatedHostCode << "Part 1: recorder array declaration\n";
    if (numConditions){
        generatedHostCode << "int " << branchRecorderArrayName << "[" << numConditions*2 << "] = {0};\n" // Branch coverage checker
            << "cl_mem d_" << branchRecorderArrayName << " = clCreateBuffer(" << clContext << ", CL_MEM_READ_WRITE, sizeof(int)*" << numConditions*2 << ", NULL, &" << errorCodeVariable << ");\n"
            << errorCodeVariable << " = clEnqueueWriteBuffer(" << clCommandQueue << ", d_" << branchRecorderArrayName << ", CL_TRUE, 0, " << numConditions*2 << "*sizeof(int)," << branchRecorderArrayName << ", 0, NULL ,NULL);\n\n";
    }
    if (numBarriers){
        generatedHostCode << "int " << barrierRecorderArrayName << "[" << numBarriers << "] = {0};\n" // Barrier divergence checker
            << "cl_mem d_" << barrierRecorderArrayName << " = clCreateBuffer(" << clContext << ", CL_MEM_READ_WRITE, sizeof(int)*" << numBarriers << ", NULL, &" << errorCodeVariable << ");\n"
            << errorCodeVariable << " = clEnqueueWriteBuffer(" << clCommandQueue << ", d_" << barrierRecorderArrayName << ", CL_TRUE, 0, " << numBarriers << "*sizeof(int)," << barrierRecorderArrayName << ", 0, NULL ,NULL);\n\n";
    }
    // Host code part 2 - set argument to kernel function
    generatedHostCode << setArgumentPartHostCode.str() << "\n";

    // Host code part 3 - get data back from GPU
    generatedHostCode << "Part 3: get back from GPU\n";
    if (numConditions){
        generatedHostCode
            << errorCodeVariable << " = clEnqueueReadBuffer(" << clCommandQueue << ", d_" << branchRecorderArrayName << ", CL_TRUE, 0, sizeof(int)*" << numConditions*2 << ", " << branchRecorderArrayName << ", 0, NULL, NULL);\n";
    }
    if (numBarriers){
        generatedHostCode
            << errorCodeVariable << " = clEnqueueReadBuffer(" << clCommandQueue << ", d_" << barrierRecorderArrayName << ", CL_TRUE, 0, sizeof(int)*" << numBarriers << ", " << barrierRecorderArrayName << ", 0, NULL, NULL);\n\n";
    }

    // Host code part 4 - print result
    generatedHostCode << "Part 4: print converage result\n"
        << "FILE *openclbc_fp;\n"
        << "char *line = NULL;\n"
        << "size_t len = 0;\n"
        << "openclbc_fp = fopen(\"" << dataFilePath << "\", \"r\");\n"
        << "if (!openclbc_fp){\n"
        << "printf(\"OpenCLBC data file not found\\n\");\n"
        << "}else{\n";
    if (numConditions){
        generatedHostCode 
            << "int openclbc_total_branches = " << numConditions*2 << ", openclbc_covered_branches = 0;\n"
            << "double openclbc_result;\n"
            << "printf(\"\\x1B[34mCondition coverage summary\\x1B[0m\\n\");\n"
            << "for (int cov_test_i = 0; cov_test_i < " << numConditions*2 << "; cov_test_i+=2){\n"
            << "  getline(&line, &len, openclbc_fp);\n"
            << "  printf(\"%s\", line);\n"
            << "  getline(&line, &len, openclbc_fp);\n"
            << "  printf(\"%s\", line);\n"
            << "  getline(&line, &len, openclbc_fp);\n"
            << "  printf(\"%s\", line);\n"
            << "  if (" << branchRecorderArrayName << "[cov_test_i]) {\n" 
            << "    printf(\"\\x1B[32mTrue branch covered\\x1B[0m\\n\");\n"
            << "    openclbc_covered_branches++;\n"
            << "  } else { \n"
            << "    printf(\"\\x1B[31mTrue branch not covered\\x1B[0m\\n\");\n"
            << "  }\n"
            << "  if (" << branchRecorderArrayName << "[cov_test_i + 1]) {\n" 
            << "    printf(\"\\x1B[32mFalse branch covered\\x1B[0m\\n\");\n"
            << "    openclbc_covered_branches++;\n"
            << "  } else { \n"
            << "    printf(\"\\x1B[31mFalse branch not covered\\x1B[0m\\n\");\n"
            << "  }\n"
            << "}\n";
    }
    if (numBarriers){
        generatedHostCode
            << "int openclbc_total_barriers = " << numBarriers << ", openclbc_faulty_barriers = 0;\n"
            << "double openclbc_barrier_result;\n"
            << "for (int cov_test_i = 0; cov_test_i < " << numBarriers << "; ++cov_test_i){\n"
            << "  getline(&line, &len, openclbc_fp);\n"
            << "  printf(\"%s\", line);\n"
            << "  getline(&line, &len, openclbc_fp);\n"
            << "  printf(\"%s\", line);\n"
            << "  if (" << barrierRecorderArrayName << "[cov_test_i]) {\n" 
            << "    printf(\"\\x1B[31mThis barrier has got a divergence\\x1B[0m\\n\");\n"
            << "    ++openclbc_faulty_barriers;\n"
            << "  } else { \n"
            << "    printf(\"\\x1B[32mThis barrier worked fine\\x1B[0m\\n\");\n"
            << "  }\n"
            << "}\n";
    }
    if (numConditions){
        generatedHostCode
            << "openclbc_result = (double)openclbc_covered_branches / (double)openclbc_total_branches *100.0;\n"
            << "printf(\"Total branch coverage: %-4.2f\\n\", openclbc_result);\n";
    }
    if (numBarriers){
        generatedHostCode
            << "openclbc_barrier_result = (double)openclbc_faulty_barriers / (double)openclbc_total_barriers *100.0;\n"
            << "printf(\"Faulty barrier rate: %-4.2f\\n\", openclbc_barrier_result);\n";
    }
    generatedHostCode
        << "}\n\n";

}

std::string HostCodeGenerator::getGeneratedHostCode(){
    return generatedHostCode.str();
}