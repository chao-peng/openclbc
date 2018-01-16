
#include <sstream>
#include <string>
#include <fstream>
#include <iostream>
#include <map>
#include <set>

#include "clang/AST/AST.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/ASTConsumers.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "clang/Tooling/Core/Replacement.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/Basic/LLVM.h"

#include "OpenCLKernelRewriter.h"
#include "Constants.h"
#include "UserConfig.h"

using namespace clang;
using namespace clang::tooling;

std::string outputFileName;
std::string outputDirectory;
std::string configFileName;
std::string kernelSourceFile;
int numAddedLines;
int numConditions; // Used for labelling if-conditions when rewriting the kernel code
int countConditions; // Used for counting if-conditions before rewriting the kernel code
std::map<int, std::string> conditionLineMap; // Line number of each condition
std::map<int, std::string> conditionStringMap; // Details of each condition
std::set<std::string> setFunctions; // A set of user-defined functions

// Variables below are used to generate host code
std::string kernel_function_name;
std::string error_code_variable;
std::string error_code_checker;
std::string cl_context;
std::string cl_command_queue;
std::string recorder_array_name;
std::string data_file_path;
std::stringstream generated_host_code;

// First AST visitor: counting if-conditions and user-defined functions
class RecursiveASTVisitorForKernelInvastigator : public RecursiveASTVisitor<RecursiveASTVisitorForKernelInvastigator> {
public:
    explicit RecursiveASTVisitorForKernelInvastigator(Rewriter &r) : myRewriter(r) {}

    // count the number of if-conditions
    bool VisitStmt(Stmt *s) {
        if (isa<IfStmt>(s)){
            countConditions++;
        }
        return true;
    }
    
    // record user-defined functions in a set
    bool VisitFunctionDecl(FunctionDecl *f) {
        SourceLocation locStart, locEnd;
        SourceRange sr;
        locStart = f->getLocStart();
        locEnd = f->getLocStart().getLocWithOffset(8);
        sr.setBegin(locStart);
        sr.setEnd(locEnd);
        
        std::string typeString = myRewriter.getRewrittenText(sr);
        if (typeString != "__kernel"){
            if (f->hasBody()){
                setFunctions.insert(f->getQualifiedNameAsString());
            }
        }
        return true;
    }

private:
    Rewriter &myRewriter;
};

class ASTConsumerForKernelInvastigator : public ASTConsumer{
public:
    ASTConsumerForKernelInvastigator(Rewriter &r) : visitor(r) {}

    bool HandleTopLevelDecl(DeclGroupRef DR) override {
        for (DeclGroupRef::iterator b = DR.begin(), e = DR.end(); b != e; ++b) {
            // Traverse the declaration using our AST visitor.
            visitor.TraverseDecl(*b);
            //(*b)->dump();
        }
    return true;
    }

private:
  RecursiveASTVisitorForKernelInvastigator visitor;
};

// Before visiting the AST, add a fake header so that clang will not complain about opencl library calls and macros
class ASTFrontendActionForKernelInvastigator : public ASTFrontendAction {
public:
    ASTFrontendActionForKernelInvastigator(){}

    virtual std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &ci, 
        StringRef file) override {
            kernelSourceFile = file.str();
            //if (!UserConfig::hasFakeHeader(kernelSourceFile)){
            //    numAddedLines = UserConfig::generateFakeHeader(configFileName, kernelSourceFile);
            //}
            myRewriter.setSourceMgr(ci.getSourceManager(), ci.getLangOpts());
            return llvm::make_unique<ASTConsumerForKernelInvastigator>(myRewriter);
        }

private:
    Rewriter myRewriter;
};

// Second and main AST visitor:
// 1. Rewrite if blocks to update the local recorder array
// 2. Add the pointer to the local recorder array as the last argument to user-defined function declaration and calls
// 3. Add a loop to the end of kernel function to update the local recorder array to the global one
// 4. Add the pointer to the global recorder array as the last argument to the kernel entry function 
class RecursiveASTVisitorForKernelRewriter : public RecursiveASTVisitor<RecursiveASTVisitorForKernelRewriter> {
public:
    explicit RecursiveASTVisitorForKernelRewriter(Rewriter &r, Rewriter &original_r) : myRewriter(r), originalRewriter (original_r){}
    
    bool VisitStmt(Stmt *s) {
        if (isa<IfStmt>(s)) {
            // Deal with If
            // Record details of this condition
            IfStmt *IfStatement = cast<IfStmt>(s);
            std::string locIfStatement = IfStatement->getLocStart().printToString(myRewriter.getSourceMgr());
            std::string conditionIfStatement = myRewriter.getRewrittenText(IfStatement->getCond()->getSourceRange());
            SourceLocation conditionStart = myRewriter.getSourceMgr().getFileLoc(IfStatement->getCond()->getLocStart());
            SourceLocation conditionEnd = myRewriter.getSourceMgr().getFileLoc(IfStatement->getCond()->getLocEnd());
            SourceRange conditionRange;
            conditionRange.setBegin(conditionStart);
            conditionRange.setEnd(conditionEnd);
            // Insert to the hashmap of line numbers of conditions
            // Line number needs to be adjusted due to possible added lines of fake header statements
            size_t p1, p2;
            p1 = locIfStatement.substr(0, locIfStatement.find_last_of(':')).find_last_of(':') + 1;
            p2 = locIfStatement.find_last_of(':');
            int newLineNumber = std::stoi(locIfStatement.substr(p1, p2-p1)) - numAddedLines;
            std::string newLocIfStatement = locIfStatement.substr(0, p1);
            newLocIfStatement.append(std::to_string(newLineNumber));
            newLocIfStatement.append(locIfStatement.substr(p2));
            conditionLineMap[numConditions] = newLocIfStatement;
            // Insert to the hashmap of text of conditions
            conditionStringMap[numConditions] = myRewriter.getRewrittenText(conditionRange);

            Stmt* Then = IfStatement->getThen();
            if(isa<CompoundStmt>(Then)) {
                // Then is a compound statement
                // Add coverage recorder to the end of the compound
                myRewriter.InsertTextAfter(
                    Then->getLocStart().getLocWithOffset(1),
                    stmtRecordCoverage(2 * numConditions)
                    );
            } else {
                // Then is a single statement
                // decorate it with {} and add coverage recorder
                // Need to be aware of a statement with macros
                SourceLocation startLoc = myRewriter.getSourceMgr().getFileLoc(
                    Then->getLocStart());
                SourceLocation endLoc = myRewriter.getSourceMgr().getFileLoc(
                    Then->getLocEnd());
                SourceRange newRange;
                newRange.setBegin(startLoc);
                newRange.setEnd(endLoc);

                std::stringstream sourcestream;
                // If there's no else block/statement, it's better add else here
                // or it might be confused with end of function and end of if
                bool hasElse = false;
                if (IfStatement->getElse()) hasElse = true;
                sourcestream << "{"
                        << stmtRecordCoverage(2 * numConditions)
                        << originalRewriter.getRewrittenText(newRange) 
                        << ";\n}";
                
                if (!hasElse){
                    sourcestream << " else { "
                        << stmtRecordCoverage(2 * numConditions + 1)
                        << "}\n";
                }
                myRewriter.ReplaceText(
                    newRange.getBegin(),
                    originalRewriter.getRewrittenText(newRange).length() + 1,
                    sourcestream.str()
                );
                if(!hasElse){
                    numConditions++;
                    return true;
                }
            }
            
            Stmt* Else = IfStatement->getElse();
            if (Else) {
                // Deal with Else
                if (isa<CompoundStmt>(Else)) {
                    // Else is a compound statement
                    // Add coverage recorder to the end of the compound
                    myRewriter.InsertTextAfter(
                        Else->getLocStart().getLocWithOffset(1),
                        stmtRecordCoverage(2 * numConditions + 1)
                        );
                } else if (isa<IfStmt>(Else)) {
                    // Else is another condition (else if)
                    std::stringstream ss;
                    ss << "{\n"
                        << stmtRecordCoverage(2 * numConditions + 1)
                        << "\n";
                    myRewriter.InsertTextAfter(
                        Else->getLocStart(),
                        ss.str()
                    );
                    myRewriter.InsertTextAfter(
                        Else->getLocEnd().getLocWithOffset(2),
                        "}\n"
                    );
                } else {
                    // Else is a single statement
                    // decorate it with {} and add coverage recorder
                    SourceLocation startLoc = myRewriter.getSourceMgr().getFileLoc(
                        Else->getLocStart());
                    SourceLocation endLoc = myRewriter.getSourceMgr().getFileLoc(
                        Else->getLocEnd());
                    SourceRange newRange;
                    newRange.setBegin(startLoc);
                    newRange.setEnd(endLoc);
                
                    std::stringstream sourcestream;
                    sourcestream << "{"
                        << stmtRecordCoverage(2 * numConditions + 1)
                        << myRewriter.getRewrittenText(newRange) 
                        << ";\n}";
                    myRewriter.ReplaceText(
                        newRange.getBegin(),
                        myRewriter.getRewrittenText(newRange).length() + 1,
                        sourcestream.str()
                    );
                }
                
            } else {
                // Else does not exist
                // Add corresponding else and coverage recorder in it
                std::stringstream newElse;
                newElse << "else {\n" 
                    << stmtRecordCoverage(2 * numConditions + 1)
                    << "}\n";
                myRewriter.InsertTextBefore(
                    IfStatement->getSourceRange().getEnd().getLocWithOffset(2),
                    newElse.str()
                );
            }
            
            numConditions++;
        } else if (isa<CallExpr>(s)){
            CallExpr *functionCall = cast<CallExpr>(s);
            SourceLocation startLoc = myRewriter.getSourceMgr().getFileLoc(
                    functionCall->getCallee()->getLocStart());
            SourceLocation endLoc = myRewriter.getSourceMgr().getFileLoc(
                    functionCall->getCallee()->getLocEnd());
            SourceRange newRange;
            newRange.setBegin(startLoc);
            newRange.setEnd(endLoc);
            std::string functionName = myRewriter.getRewrittenText(newRange);
            functionName = originalRewriter.getRewrittenText(functionCall->getCallee()->getSourceRange());
            if (setFunctions.find(functionName) != setFunctions.end()){
                myRewriter.InsertTextAfter(
                    functionCall->getLocEnd().getLocWithOffset(0),
                    localRecorderArgument()
                );
            }
        }
        
        return true;
    }

    bool VisitFunctionDecl(FunctionDecl *f){
        // If there exist branches
        // Need to do this because if We do nothing with the original code, rewriter buffer will be empty
        if (countConditions != 0){
            // Need to deal with 4 possible types of function declarations
            // 1. __kernel function - add both __global parameter and __local array definition
            // 2. __kernel function prototype - add _global parameter
            // 3. non-kernel function - add _local array parameter
            // 4. non-kernel function prototype - same of 3

            // If it is a kernel function, typeString = "__kernel"
            SourceLocation locStart, locEnd;
            SourceRange sr;
            locStart = f->getLocStart();
            locEnd = f->getLocStart().getLocWithOffset(8);
            sr.setBegin(locStart);
            sr.setEnd(locEnd);
            std::string typeString = myRewriter.getRewrittenText(sr);
            std::string functionName = f->getQualifiedNameAsString();
            if (typeString == "__kernel"){
                if (f->hasBody()){
                    // add global recorder array as argument to function definition
                    SourceLocation loc = f->getBody()->getLocStart().getLocWithOffset(-2);
                    myRewriter.InsertTextAfter(loc, declRecorder());

                    // define recorder array as __local array
                    loc = f->getBody()->getLocStart().getLocWithOffset(2);
                    myRewriter.InsertTextAfter(loc, declLocalRecorder());
                    
                    // update local recorder to global recorder array
                    loc = f->getBody()->getLocEnd();
                    myRewriter.InsertTextAfter(loc, stmtUpdateGlobalRecorder());

                    // Host code generator part 2: Set argument
                    int argumentLocation = f->param_size();
                    generated_host_code << "\x1B[34mopenclbc - set argument to kernel function\x1B[0m\n"
                        << error_code_checker << "( clSetKernelArg(" << functionName << ", " << argumentLocation << ", sizeof(cl_mem), &d_" << recorder_array_name << "));\n\n";

                }
                else {
                    // add global recorder array as argument to function prototype
                    SourceLocation loc = f->getLocEnd();
                    myRewriter.InsertTextAfter(loc.getLocWithOffset(-2), declRecorder());
                }
            } else {
                // Not a kernel function
                if (f->hasBody()){
                    SourceLocation loc = f->getBody()->getLocStart().getLocWithOffset(-2);
                    myRewriter.InsertTextAfter(loc, declLocalRecorderArgument());
                } else {
                    SourceLocation loc = f->getLocEnd();
                    myRewriter.InsertTextAfter(loc.getLocWithOffset(-2), declLocalRecorderArgument());
                }
            }
        } else {
            // No branches
            SourceLocation loc = f->getLocStart();
            myRewriter.InsertTextBefore(loc, "/*No branches*/ \n");
        }
        return true;
    }

private:
    Rewriter &myRewriter;
    Rewriter &originalRewriter;

    std::string stmtRecordCoverage(const int& id){
        std::stringstream ss;
        // old implementation
        // ss << kernel_rewriter_constants::COVERAGE_RECORDER_NAME << "[" << id << "] = true;\n";
        // replaced by atomic_or operation to avoid data race
        ss << "\natomic_or(&" << kernel_rewriter_constants::LOCAL_COVERAGE_RECORDER_NAME << "[" << id << "], 1);\n";
        return ss.str();
    }

    std::string declRecorder(){
        std::stringstream ss;
        ss << ", __global int* " << kernel_rewriter_constants::GLOBAL_COVERAGE_RECORDER_NAME;
        return ss.str();
    }

    std::string declLocalRecorder(){
        std::stringstream ss;
        ss << "__local int " << kernel_rewriter_constants::LOCAL_COVERAGE_RECORDER_NAME << "[" << 2 * countConditions << "];\n";
        return ss.str();
    }

    std::string declLocalRecorderArgument(){
        std::stringstream ss;
        ss << ", __local int* " << kernel_rewriter_constants::LOCAL_COVERAGE_RECORDER_NAME;
        return ss.str();
    }

    std::string localRecorderArgument(){
        std::stringstream ss;
        ss << ", " << kernel_rewriter_constants::LOCAL_COVERAGE_RECORDER_NAME;
        return ss.str();
    }

    std::string stmtUpdateGlobalRecorder(){
        std::stringstream ss;
        ss << "for (int update_recorder_i = 0; update_recorder_i < " << (countConditions*2) << "; update_recorder_i++) { \n";
        ss << "  atomic_or(&" << kernel_rewriter_constants::GLOBAL_COVERAGE_RECORDER_NAME << "[update_recorder_i], " << kernel_rewriter_constants::LOCAL_COVERAGE_RECORDER_NAME << "[update_recorder_i]); \n";
        ss << "}\n";
        return ss.str();
    }
};

class ASTConsumerForKernelRewriter : public ASTConsumer{
public:
    ASTConsumerForKernelRewriter(Rewriter &r, Rewriter &original_r) : visitor(r, original_r) {}

    bool HandleTopLevelDecl(DeclGroupRef DR) override {
        for (DeclGroupRef::iterator b = DR.begin(), e = DR.end(); b != e; ++b) {
            // Traverse the declaration using our AST visitor.
            visitor.TraverseDecl(*b);
            //(*b)->dump();
        }
    return true;
    }

private:
  RecursiveASTVisitorForKernelRewriter visitor;
};

class ASTFrontendActionForKernelRewriter : public ASTFrontendAction {
public:
    ASTFrontendActionForKernelRewriter(){}

    void EndSourceFileAction() override {
        const RewriteBuffer *buffer = myRewriter.getRewriteBufferFor(myRewriter.getSourceMgr().getMainFileID());
        if (buffer == NULL){
            llvm::outs() << "Rewriter buffer is null. Cannot write in file.\n";
            return;
        }
        std::string rewriteBuffer = std::string(buffer->begin(), buffer->end());
        std::string source = "";
        std::string line;
        std::istringstream bufferStream(rewriteBuffer);
        while(getline(bufferStream, line)){
            source.append(line);
            source.append("\n");
        }

        // Write modified kernel source code
        std::ofstream fileWriter;
        fileWriter.open(outputFileName);
        fileWriter << source;
        fileWriter.close();
        
        // Write data file
        std::string dataFileName = outputFileName + ".dat";
        data_file_path = dataFileName;
        std::stringstream outputBuffer;
        fileWriter.open(dataFileName);
        for (int i = 0; i < numConditions; i++){
            outputBuffer << "Condition ID: " << i << "\n";
            outputBuffer << "Source code line: " << conditionLineMap[i] << "\n";
            outputBuffer << "Condition: " << conditionStringMap[i] << "\n";
        }
        outputBuffer << "\n";
        fileWriter << outputBuffer.str();
        fileWriter.close();

        if (UserConfig::hasFakeHeader(kernelSourceFile)){
            UserConfig::removeFakeHeader(kernelSourceFile);
        }

        if (UserConfig::hasFakeHeader(outputFileName)){
            UserConfig::removeFakeHeader(outputFileName);
        }
    }

    virtual std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &ci, 
        StringRef file) override {
            std::string inputFileName = file.str();
            outputFileName = outputFileName.append(inputFileName.substr(inputFileName.find_last_of("/") + 1, inputFileName.size() - inputFileName.find_last_of("/") - 1));
            myRewriter.setSourceMgr(ci.getSourceManager(), ci.getLangOpts());
            originalRewriter.setSourceMgr(ci.getSourceManager(), ci.getLangOpts());
            return llvm::make_unique<ASTConsumerForKernelRewriter>(myRewriter, originalRewriter);
    }

private:
    Rewriter myRewriter;
    Rewriter originalRewriter;
    // need original rewriter to retrieve correct text from original code
};

std::map<int, std::string> rewriteOpenclKernel(ClangTool* tool, std::string newOutputDirectory, int newNumAddedLines, UserConfig* userConfig) {
    numConditions = 0;
    countConditions = 0;
    numAddedLines = newNumAddedLines;
    outputDirectory = newOutputDirectory;
    outputFileName = newOutputDirectory;

    kernel_function_name = userConfig->getValue("kernel_function_name");
    recorder_array_name = kernel_function_name + "_branch_coverage_recorder";
    cl_context = userConfig->getValue("cl_context");
    error_code_variable = userConfig->getValue("error_code_variable");
    error_code_checker = userConfig->getValue("error_code_checker");
    cl_command_queue = userConfig->getValue("cl_command_queue");

    tool->run(newFrontendActionFactory<ASTFrontendActionForKernelInvastigator>().get());    

    // Host code generator part 1: Declare branch coverage recorder array
    generated_host_code << "\x1B[34mopenclbc - recorder array declaration\x1B[0m\n"
        << "int " << recorder_array_name << "[" << countConditions*2 << "] = {0};\n"
        << "cl_mem d_" << recorder_array_name << " = clCreateBuffer(" << cl_context << ", CL_MEM_READ_WRITE, sizeof(int)*" << countConditions*2 << ", NULL, &" << error_code_variable << ");\n"
        << error_code_checker << "(clEnqueueWriteBuffer(" << cl_command_queue << ", d_" << recorder_array_name << ", CL_TRUE, 0, " << countConditions*2 << "*sizeof(int)," << recorder_array_name << ", 0, NULL ,NULL));\n\n";

    tool->run(newFrontendActionFactory<ASTFrontendActionForKernelRewriter>().get());

    // Host code generator part3: Get array back from GPU
    generated_host_code << "\x1B[34mopenclbc - get back from GPU\x1B[0m\n"
        << error_code_checker << "(clEnqueueReadBuffer(" << cl_command_queue << ", d_" << recorder_array_name << ", CL_TRUE, 0, sizeof(int)*" << countConditions*2 << ", " << recorder_array_name << ", 0, NULL, NULL));\n\n";

    // Host code generator part4: Print result
    generated_host_code << "\x1B[34mopenclbc - print converage result\x1B[0m\n" 
        << "int openclbc_total_branches = " << countConditions*2 << ", openclbc_covered_branches = 0;\n"
        << "double openclbc_result;\n"
        << "FILE *openclbc_fp;\n"
        << "char *line = NULL;\n"
        << "size_t len = 0;\n"
        << "openclbc_fp = fopen(\"" << data_file_path << "\", \"r\");\n"
        << "if (openclbc_fp){"
        << "printf(\"\\x1B[34mCondition coverage summary\\x1B[0m\\n\");\n"
        << "for (int cov_test_i = 0; cov_test_i < " << countConditions*2 << "; cov_test_i+=2){\n"
        << "  getline(&line, &len, openclbc_fp);\n"
        << "  printf(\"%s\", line);\n"
        << "  getline(&line, &len, openclbc_fp);\n"
        << "  printf(\"%s\", line);\n"
        << "  getline(&line, &len, openclbc_fp);\n"
        << "  printf(\"%s\", line);\n"
        << "  if (" << recorder_array_name << "[cov_test_i]) {\n" 
        << "    printf(\"\\x1B[32mTrue branch covered\\x1B[0m\\n\");\n"
        << "    openclbc_covered_branches++;\n"
        << "  } else { \n"
        << "    printf(\"\\x1B[31mTrue branch not covered\\x1B[0m\\n\");\n"
        << "  }\n"
        << "  if (" << recorder_array_name << "[cov_test_i + 1]) {\n" 
        << "    printf(\"\\x1B[32mFalse branch covered\\x1B[0m\\n\");\n"
        << "    openclbc_covered_branches++;\n"
        << "  } else { \n"
        << "    printf(\"\\x1B[31mFalse branch not covered\\x1B[0m\\n\");\n"
        << "  }\n"
        << "}\n"
        << "fclose(openclbc_fp);\n"
        << "openclbc_result = (double)openclbc_covered_branches / (double)openclbc_total_branches * 100.0;\n"
        << "printf(\"Total coverage %-4.2f\\n\", openclbc_result);\n"
        << "} else {"
        << "  printf(\"OpenCLBC data file not found\\n\");\n"
        << "}\n\n";

    if (!userConfig->isEmpty()){
        std::cout << generated_host_code.str() << "Host code above has also been written in the output directory\n";
        std::string hostCodeFile = outputDirectory + "hostcode.txt";
        std::ofstream hostCodeWriter(hostCodeFile);
        hostCodeWriter << generated_host_code.str();
        hostCodeWriter.close();
    }

    return conditionLineMap;
}
