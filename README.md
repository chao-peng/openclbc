# OpenCLBC: Testing OpenCL Kernel Branch Coverage

## Introduction

Based on Clang Libtooling technique, OpenCLBC is developped to insturment OpenCL kernel source code and check branch coverage of the program.

## Folder Structure

* **src** Source code
* **test** Example tests for evaluation of this tool

## Build

1. To build and use this tool, please first follow the instructions on how to build Clang/LLVM on your machine

    [http://clang.llvm.org/docs/LibASTMatchersTutorial.html](http://clang.llvm.org/docs/LibASTMatchersTutorial.html)

2. Clone OpenCLBC to `~/clang-llvm/llvm/tools/clang/tools/extra`

3. Add the following line to `~/clang-llvm/llvm/tools/clang/tools/extra/CMakeLists.txt`

```
    add_subdirectory(openclbc)
```

4. Build OpenCLBC

```bash
    cd ~/clang-llvm/build
    ninja
```

## Usage

We assume that your kernel source code is written following the [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html). If not, please use clang-format command to restyle your code.

To run OpenCLBC on your kernel source code:

```bash
    ~/clang-llvm/llvm/build/bin/openclbc yourkernelfile.cl -o outpurdirectory
```

After running this command, the instrumented kernel source code along with our profiling datasets will be written to the directory you provide.
