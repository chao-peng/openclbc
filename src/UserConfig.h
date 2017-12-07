#ifndef OPENCLBC_USER_CONFIG_H
#define OPENCLBC_USER_CONFIG_H

#include <string>
#include <set>

class UserConfig{
private:
    std::set<std::string> setMacro;
    int numAdditionalLines;

public:
    
    //Constructor using path to config file
    UserConfig(std::string configFile);

    //Generate the fake header with macros specified by the user
    std::string generateFakeHeader();

    //Get number of lines of added fake header statements 
    int getNumAdditionalLines();
};

#endif