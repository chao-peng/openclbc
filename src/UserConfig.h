#ifndef OPENCLBC_USER_CONFIG_H
#define OPENCLBC_USER_CONFIG_H

#include <string>
#include <set>

class UserConfig{
public:
    
    //Generate the fake header with macros specified by the user
    static int generateFakeHeader(std::string configFileName, std::string kernelFileName);

    static int removeFakeHeader(std::string kernelFileName);

    static bool hasFakeHeader(std::string kernelFileName);

    static std::set<std::string> getValues(std::string configFileName, std::string key);
};

#endif