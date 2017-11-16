
#include <sstream>
#include <string>
#include <fstream>
#include <iostream>
#include <map>

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

#include "Constants.h"

using namespace clang;
using namespace clang::tooling;

std::string outputFileName;
int numConditions;
std::map<int, std::string> branchMap;

class ASTVisitorForKernel : public RecursiveASTVisitor<ASTVisitorForKernel> {
public:
    explicit ASTVisitorForKernel(Rewriter &r) : myRewriter(r) {}
    bool VisitStmt(Stmt *s) {
        if (isa<IfStmt>(s)) {
            // Deal with If
            IfStmt *IfStatement = cast<IfStmt>(s);
            std::string locIfStatement = IfStatement->getLocStart().printToString(myRewriter.getSourceMgr());
            branchMap[numConditions] = locIfStatement;

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
                    myRewriter.getRewrittenText(Then->getSourceRange()).length() + 1,
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
                        myRewriter.getRewrittenText(Else->getSourceRange()).length() + 1,
                        sourcestream.str()
                    );
                }
                
            } else {
                // Else does not exist
                // Add corresponding else and coverage recorder in it
                std::stringstream newElse;
                newElse << "else {\n" 
                    << stmtRecordCoverage(2 * numConditions + 1)
                    << "\n}";
                myRewriter.InsertTextAfter(
                    IfStatement->getSourceRange().getEnd().getLocWithOffset(2),
                    newElse.str()
                );
            }
            
            numConditions++;
        }
        
        return true;
    }

    bool VisitFunctionDecl(FunctionDecl *f){
        if (f->hasBody()){
            SourceLocation loc = f->getBody()->getLocStart().getLocWithOffset(-2);
            myRewriter.InsertTextAfter(loc, declRecorder());
        } else {
            SourceLocation loc = f->getLocEnd();
            myRewriter.InsertTextAfter(loc, declRecorder());
        }
        return true;
    }

private:
    Rewriter &myRewriter;

    std::string stmtRecordCoverage(const int& id){
        std::stringstream ss;
        ss << kernel_rewriter_constants::COVERAGE_RECORDER_NAME << "[" << id << "] = true;\n";
        return ss.str();
    }

    std::string declRecorder(){
        std::stringstream ss;
        ss << ", __global int* " << kernel_rewriter_constants::COVERAGE_RECORDER_NAME;
        return ss.str();
    }
};

class ASTConsumerForKernel : public ASTConsumer{
public:
    ASTConsumerForKernel(Rewriter &r) : visitor(r) {}

    bool HandleTopLevelDecl(DeclGroupRef DR) override {
        for (DeclGroupRef::iterator b = DR.begin(), e = DR.end(); b != e; ++b) {
            // Traverse the declaration using our AST visitor.
            visitor.TraverseDecl(*b);
            (*b)->dump();
        }
    return true;
    }

private:
  ASTVisitorForKernel visitor;
};

class FrontendActionForKernel : public ASTFrontendAction {
public:
    FrontendActionForKernel(){}

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

        std::ofstream fileWriter;
        fileWriter.open(outputFileName);
        fileWriter << source;
        fileWriter.close();
        
    }

    virtual std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &ci, 
        StringRef file) override {
            myRewriter.setSourceMgr(ci.getSourceManager(), ci.getLangOpts());
            return llvm::make_unique<ASTConsumerForKernel>(myRewriter);
        }

private:
    Rewriter myRewriter;
};

std::map<int, std::string> rewriteOpenclKernel(ClangTool* tool, std::string newOutputFileName) {
    numConditions = 0;
    outputFileName = newOutputFileName;
    tool->run(newFrontendActionFactory<FrontendActionForKernel>().get());
    return branchMap;
}
