#ifndef OPENCLBC_USER_CONFIG_H
#define OPENCLBC_USER_CONFIG_H

#include <string>
#include <set>

class UserConfig{
private:
    std::string userConfigFileName;
public:
    
    UserConfig(std::string filename);

    //Generate the fake header with macros specified by the user
    int generateFakeHeader(std::string kernelFileName);

    static int removeFakeHeader(std::string kernelFileName);

    static bool hasFakeHeader(std::string kernelFileName);

    std::set<std::string> getValues(std::string key);

    std::string getValue(std::string key);
};

#endif