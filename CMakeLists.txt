set(LLVM_LINK_COMPONENTS
    Support
)
        
add_clang_executable(openclbc
    src/Constants.h
    src/HostCodeInvastigator.cpp
    src/HostCodeInvastigator.h
    src/Main.cpp
    src/OpenCLKernelRewriter.cpp
    src/OpenCLKernelRewriter.h
    src/UserConfig.cpp
    src/UserConfig.h)
        
target_link_libraries(openclbc
    clangAST
    clangASTMatchers
    clangBasic
    clangFrontend
    clangTooling)