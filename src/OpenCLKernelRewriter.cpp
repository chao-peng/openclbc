
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
int numConditions; // Used for labelling if-conditions when rewriting the kernel code
int countConditions; // Used for counting if-conditions before rewriting the kernel code
std::map<int, std::string> conditionLineMap; // Line number of each condition
std::map<int, std::string> conditionStringMap; // Details of each condition
std::set<std::string> setFunctions; // A set of user-defined functions

class RecursiveASTVisitorForKernelInvastigator : public RecursiveASTVisitor<RecursiveASTVisitorForKernelInvastigator> {
public:
    explicit RecursiveASTVisitorForKernelInvastigator(Rewriter &r) : myRewriter(r) {}

    bool VisitStmt(Stmt *s) {
        if (isa<IfStmt>(s)){
            countConditions++;
        }
        return true;
    }
    
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

class ASTFrontendActionForKernelInvastigator : public ASTFrontendAction {
public:
    ASTFrontendActionForKernelInvastigator(){}

    virtual std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &ci, 
        StringRef file) override {
            if (!hasFakeHeader(file.str())){
                addFakeHeader(file.str());
            }
            myRewriter.setSourceMgr(ci.getSourceManager(), ci.getLangOpts());
            return llvm::make_unique<ASTConsumerForKernelInvastigator>(myRewriter);
        }

private:
    Rewriter myRewriter;

    bool hasFakeHeader(std::string inputFileName){
        std::ifstream kernelFileStream(inputFileName);
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

    void addFakeHeader(std::string inputFileName){
        std::stringstream kernelSource;
        std::string line;
        std::fstream kernelFileStream(inputFileName, std::ios::in);
        UserConfig myConfig(configFileName);

        kernelSource << myConfig.generateFakeHeader();

        while(std::getline(kernelFileStream, line)){
            kernelSource<<line<<"\n";
        }
        kernelFileStream.close();

        std::fstream newKernelFileStream(inputFileName, std::ios::out);
        newKernelFileStream << kernelSource.str();
        newKernelFileStream.close();
    }
};

class RecursiveASTVisitorForKernelRewriter : public RecursiveASTVisitor<RecursiveASTVisitorForKernelRewriter> {
public:
    explicit RecursiveASTVisitorForKernelRewriter(Rewriter &r, Rewriter &original_r) : myRewriter(r), originalRewriter (original_r){}
    
    bool VisitStmt(Stmt *s) {
        if (isa<IfStmt>(s)) {
            // Deal with If
            IfStmt *IfStatement = cast<IfStmt>(s);
            std::string locIfStatement = IfStatement->getLocStart().printToString(myRewriter.getSourceMgr());
            std::string conditionIfStatement = myRewriter.getRewrittenText(IfStatement->getCond()->getSourceRange());
            SourceLocation conditionStart = myRewriter.getSourceMgr().getFileLoc(IfStatement->getCond()->getLocStart());
            SourceLocation conditionEnd = myRewriter.getSourceMgr().getFileLoc(IfStatement->getCond()->getLocEnd());
            SourceRange conditionRange;
            conditionRange.setBegin(conditionStart);
            conditionRange.setEnd(conditionEnd);
            conditionLineMap[numConditions] = locIfStatement;
            conditionStringMap[numConditions] = myRewriter.getRewrittenText(conditionRange);

            Stmt* Then = IfStatement->getThen();
            if(isa<CompoundStmt>(Then)) {
                // Then is a compound statement
                // Add coverage recorder to the end of the compound
                myRewriter.InsertTextBefore(
                    Then->getLocEnd(),
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
                sourcestream << "{\n"
                        << myRewriter.getRewrittenText(newRange) 
                        << ";\n"
                        << stmtRecordCoverage(2 * numConditions)
                        << "}\n";
                myRewriter.ReplaceText(
                    newRange.getBegin(),
                    myRewriter.getRewrittenText(newRange).length() + 1,
                    sourcestream.str()
                );
            }
            
            Stmt* Else = IfStatement->getElse();
            if (Else) {
                // Deal with Else
                if (isa<CompoundStmt>(Else)) {
                    // Else is a compound statement
                    // Add coverage recorder to the end of the compound
                    myRewriter.InsertTextBefore(
                        Else->getLocEnd(),
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
                    sourcestream << "{\n"
                        << myRewriter.getRewrittenText(newRange) 
                        << ";\n"
                        << stmtRecordCoverage(2 * numConditions + 1)
                        << "}";
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
                    << "}";
                myRewriter.InsertTextAfter(
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
            std::cout << "[function call] " << functionCall->getLocStart().printToString(originalRewriter.getSourceMgr()) << "\n" << functionName << "\n";
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
                    myRewriter.InsertTextBefore(loc, stmtUpdateGlobalRecorder());
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
        ss << "atomic_or(&" << kernel_rewriter_constants::LOCAL_COVERAGE_RECORDER_NAME << "[" << id << "], 1);\n";
        return ss.str();
    }

    std::string declRecorder(){
        std::stringstream ss;
        ss << ", __global int* " << kernel_rewriter_constants::GLOBAL_COVERAGE_RECORDER_NAME;
        return ss.str();
    }

    std::string declLocalRecorder(){
        std::stringstream ss;
        ss << "__local int* " << kernel_rewriter_constants::LOCAL_COVERAGE_RECORDER_NAME << "[" << 2 * countConditions << "];\n";
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
        std::stringstream outputBuffer;
        fileWriter.open(dataFileName);
        for (int i = 0; i < numConditions; i++){
            outputBuffer << "Condition ID: " << i << "\n";
            outputBuffer << "Source code line: " << conditionLineMap[i] << "\n";
            outputBuffer << "Condition: " << conditionStringMap[i] << "\n";
        }
        /*
        auto itLineMap = conditionLineMap.begin();
        auto itStringMap = conditionStringMap.begin();
        while (itLineMap != conditionLineMap.end() && itStringMap != conditionStringMap.end()){
            outputBuffer << itLineMap->second << "\n";
            outputBuffer << itStringMap->second << "\n";
            itLineMap++;
            itStringMap++;
        }
        */
        outputBuffer << "\n";
        fileWriter << outputBuffer.str();
        fileWriter.close();
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

std::map<int, std::string> rewriteOpenclKernel(ClangTool* tool, std::string newOutputDirectory, std::string userConfigFileName) {
    numConditions = 0;
    countConditions = 0;
    outputDirectory = newOutputDirectory;
    outputFileName = newOutputDirectory;
    configFileName = userConfigFileName;
    tool->run(newFrontendActionFactory<ASTFrontendActionForKernelInvastigator>().get());    
    tool->run(newFrontendActionFactory<ASTFrontendActionForKernelRewriter>().get());
    return conditionLineMap;
}
