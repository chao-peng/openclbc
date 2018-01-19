
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
#include "HostCodeGenerator.h"

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

int numBarriers;
int countBarriers;
std::map<int, std::string> barrierLineMap;

// Variables below are used to generate host code
HostCodeGenerator hostCodeGenerator;

// First AST visitor: counting if-conditions and user-defined functions
class RecursiveASTVisitorForKernelInvastigator : public RecursiveASTVisitor<RecursiveASTVisitorForKernelInvastigator> {
public:
    explicit RecursiveASTVisitorForKernelInvastigator(Rewriter &r) : myRewriter(r) {}

    // count the number of if-conditions
    bool VisitStmt(Stmt *s) {
        if (isa<IfStmt>(s)){
            countConditions++;
        }else if (isa<CallExpr>(s)){
            CallExpr *functionCall = cast<CallExpr>(s);
            std::string functionName = myRewriter.getRewrittenText(functionCall->getCallee()->getSourceRange());
            if (functionName == "barrier") {
                countBarriers++;
            }
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
            conditionLineMap[numConditions] = correctSourceLine(locIfStatement, numAddedLines);
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
            if (functionName == "barrier") {
                //Since people usually use OpenCL pre-defined macros as the argument of barrier
                //It's better to use SourceMgr getFileLoc here to retrieve the argument
                std::string locBarrierCall = functionCall->getLocStart().printToString(myRewriter.getSourceMgr());
                barrierLineMap[numBarriers] = correctSourceLine(locBarrierCall, numAddedLines);

                Expr* barrierArg = functionCall->getArg(0);
                std::stringstream newBarrierCall;
                SourceLocation barrierArgStartLoc = myRewriter.getSourceMgr().getFileLoc(barrierArg->getLocStart());
                SourceLocation barrierArgEndLoc = myRewriter.getSourceMgr().getFileLoc(barrierArg->getLocEnd());
                SourceRange barrierArgRange;
                barrierArgRange.setBegin(barrierArgStartLoc);
                barrierArgRange.setEnd(barrierArgEndLoc);
                newBarrierCall << "OCL_NEW_BARRIER(" << numBarriers << "," << myRewriter.getRewrittenText(barrierArgRange) << ")";
                myRewriter.ReplaceText(functionCall->getSourceRange(), newBarrierCall.str());

                numBarriers++;
            }
        }
        
        return true;
    }

    bool VisitFunctionDecl(FunctionDecl *f){
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
        bool needComma = f->getNumParams() == 0? false: true;
        if (typeString == "__kernel"){
            if (f->hasBody()){
                // add global recorder array as argument to function definition
                SourceRange funcSourceRange = f->getSourceRange();
                std::string funcSourceText = myRewriter.getRewrittenText(funcSourceRange);
                std::string funcFirstLine = funcSourceText.substr(0, funcSourceText.find_first_of('{'));
                unsigned offset = funcFirstLine.find_last_of(')');
                SourceLocation loc = f->getLocStart().getLocWithOffset(offset);
                myRewriter.InsertTextAfter(loc, declRecorder(needComma));

                // define recorder array as __local array
                loc = f->getBody()->getLocStart().getLocWithOffset(1);
                myRewriter.InsertTextAfter(loc, declLocalRecorder());
                
                // update local recorder to global recorder array
                if (countConditions){
                    loc = f->getBody()->getLocEnd();
                    myRewriter.InsertTextAfter(loc, stmtUpdateGlobalRecorder());
                }

                // Host code generator part 2: Set argument
                int argumentLocation = f->param_size();
                hostCodeGenerator.setArgument(functionName, argumentLocation);

            }
            else {
                // add global recorder array as argument to function prototype
                SourceLocation loc = f->getLocEnd();
                myRewriter.InsertTextBefore(loc, declRecorder(needComma));
            }
        } else {
            // Not a kernel function
            if (f->hasBody()){
                // If it is a function definition
                SourceRange funcSourceRange = f->getSourceRange();
                std::string funcSourceText = myRewriter.getRewrittenText(funcSourceRange);
                std::string funcFirstLine = funcSourceText.substr(0, funcSourceText.find_first_of('{'));
                unsigned offset = funcFirstLine.find_last_of(')');
                SourceLocation loc = f->getLocStart().getLocWithOffset(offset);
                myRewriter.InsertTextAfter(loc, declLocalRecorderArgument(needComma));
            } else {
                // If it is a function declaration without definition
                SourceLocation loc = f->getLocEnd();
                myRewriter.InsertTextBefore(loc, declLocalRecorderArgument(needComma));
            }
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

    std::string declRecorder(bool needComma=true){
        std::stringstream ss;
        if (needComma) ss << ", ";
        if (countConditions){
            ss << "__global int* " << kernel_rewriter_constants::GLOBAL_COVERAGE_RECORDER_NAME;
            if (countBarriers){
                ss << ", __global int* " << kernel_rewriter_constants::GLOBAL_BARRIER_DIVERFENCE_RECORDER_NAME;
            }
        } else {
            if (countBarriers){
                ss << "__global int* " << kernel_rewriter_constants::GLOBAL_BARRIER_DIVERFENCE_RECORDER_NAME;
            }
        }
        return ss.str();
    }

    std::string declLocalRecorder(){
        std::stringstream ss;
        if (countConditions){
            ss << "__local int " << kernel_rewriter_constants::LOCAL_COVERAGE_RECORDER_NAME << "[" << 2 * countConditions << "];\n";
        }
        if (countBarriers){
            ss << "__local int " << kernel_rewriter_constants::LOCAL_BARRIER_COUNTER_NAME << "[" << countBarriers << "];\n";
        }
        return ss.str();
    }

    std::string declLocalRecorderArgument(bool needComma=true){
        std::stringstream ss;
        if (needComma){
            ss << ", ";
        }
        if (countConditions){
            ss << "__local int* " << kernel_rewriter_constants::LOCAL_COVERAGE_RECORDER_NAME;
            if (countBarriers){
                ss << ", __global int* " << kernel_rewriter_constants::GLOBAL_BARRIER_DIVERFENCE_RECORDER_NAME
                    << ", __local int* " << kernel_rewriter_constants::LOCAL_BARRIER_COUNTER_NAME;
            }
        } else {
            if (countBarriers){
                ss << "__global int* " << kernel_rewriter_constants::GLOBAL_BARRIER_DIVERFENCE_RECORDER_NAME
                    << ", __local int* " << kernel_rewriter_constants::LOCAL_BARRIER_COUNTER_NAME;
            }
        }
        return ss.str();
    }

    std::string localRecorderArgument(){
        std::stringstream ss;
        if (countConditions){
            ss << ", " << kernel_rewriter_constants::LOCAL_COVERAGE_RECORDER_NAME;
        }
        if (countBarriers){
            ss << ", " << kernel_rewriter_constants::GLOBAL_BARRIER_DIVERFENCE_RECORDER_NAME
                    << ", " << kernel_rewriter_constants::LOCAL_BARRIER_COUNTER_NAME;
        }
        return ss.str();
    }

    std::string stmtUpdateGlobalRecorder(){
        std::stringstream ss;
        ss << "for (int update_recorder_i = 0; update_recorder_i < " << (countConditions*2) << "; update_recorder_i++) { \n";
        ss << "  atomic_or(&" << kernel_rewriter_constants::GLOBAL_COVERAGE_RECORDER_NAME << "[update_recorder_i], " << kernel_rewriter_constants::LOCAL_COVERAGE_RECORDER_NAME << "[update_recorder_i]); \n";
        ss << "}\n";
        return ss.str();
    }

    std::string correctSourceLine(std::string originalSourceLine, int offset){
        size_t p1, p2;
        p1 = originalSourceLine.substr(0, originalSourceLine.find_last_of(':')).find_last_of(':') + 1;
        p2 = originalSourceLine.find_last_of(':');
        int newLineNumber = std::stoi(originalSourceLine.substr(p1, p2-p1)) - offset;
        std::string neworiginalSourceLine = originalSourceLine.substr(0, p1);
        neworiginalSourceLine.append(std::to_string(newLineNumber));
        neworiginalSourceLine.append(originalSourceLine.substr(p2));
        return neworiginalSourceLine;
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

        if (countBarriers){
            source.append(kernel_rewriter_constants::NEW_BARRIER_MACRO);
            source.append("\n");
        }

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
        hostCodeGenerator.generateHostCode(dataFileName);
        std::stringstream outputBuffer;
        fileWriter.open(dataFileName);
        for (int i = 0; i < numConditions; i++){
            outputBuffer << "Condition ID: " << i << "\n";
            outputBuffer << "Source code line: " << conditionLineMap[i] << "\n";
            outputBuffer << "Condition: " << conditionStringMap[i] << "\n";
        }
        for (int i = 0; i < countBarriers; i++){
            outputBuffer << "Barrier ID: " << i << "\n";
            outputBuffer << "Source code line: " << barrierLineMap[i] << "\n";
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

int rewriteOpenclKernel(ClangTool* tool, std::string newOutputDirectory, UserConfig* userConfig) {
    numConditions = 0;
    countConditions = 0;
    countBarriers = 0;
    numBarriers = 0;
    numAddedLines = userConfig->getNumAddedLines();
    outputDirectory = newOutputDirectory;
    outputFileName = newOutputDirectory;

    tool->run(newFrontendActionFactory<ASTFrontendActionForKernelInvastigator>().get());    

    if (countConditions == 0 && countBarriers == 0){
        return error_code::NO_NEED_TO_TEST_COVERAGE;
    }

    hostCodeGenerator.initialise(userConfig, countConditions, countBarriers);

    tool->run(newFrontendActionFactory<ASTFrontendActionForKernelRewriter>().get());

    if (!userConfig->isEmpty()){
        std::cout << "Referable host code has been written in the output directory\n";
        std::string hostCodeFile = outputDirectory + "hostcode.txt";
        std::ofstream hostCodeWriter(hostCodeFile);
        hostCodeWriter << hostCodeGenerator.getGeneratedHostCode();
        hostCodeWriter.close();
    }

    return error_code::STATUS_OK;
}
