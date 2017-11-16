
#include <sstream>
#include <string>
#include <map>
#include <iostream>

#include "clang/AST/AST.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/ASTConsumers.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/Basic/LLVM.h"

#include "Constants.h"

using namespace clang;
using namespace clang::tooling;

int numKernelFiles;
std::map<int, std::string> mapKernelFiles;

class ASTVisitorForHostInvastigator : public RecursiveASTVisitor<ASTVisitorForHostInvastigator>{
public:
    explicit ASTVisitorForHostInvastigator(Rewriter &r) : myRewriter(r) {}

    bool VisitStmt(Stmt *s){
        if (isa<Expr>(s)){
            Expr* expr = cast<Expr>(s);
            if (isa<CallExpr>(expr)){
                CallExpr *functionCall = cast<CallExpr>(expr);
                std::string functionName = myRewriter.getRewrittenText(functionCall->getCallee()->getSourceRange());
                if (functionName.find("loadProgram")){
                    Expr* argument = functionCall->getArg(0);
                    std::string kernelFile = myRewriter.getRewrittenText(argument->getSourceRange());
                    mapKernelFiles[numKernelFiles] = kernelFile.substr(1, kernelFile.size()-2);
                    std::cout << "xxxx [" << mapKernelFiles[numKernelFiles] << "]\n";
                    numKernelFiles ++;
                }
            }
        }
        return true;
    }

private:
    Rewriter &myRewriter;
    
};

class ASTConsumerForHostInvastigator : public ASTConsumer{
public:
    ASTConsumerForHostInvastigator(Rewriter &r) : visitor(r) {}

    bool HandleTopLevelDecl(DeclGroupRef DR) override {
    for (DeclGroupRef::iterator b = DR.begin(), e = DR.end(); b != e; ++b) {
      // Traverse the declaration using our AST visitor.
      visitor.TraverseDecl(*b);
      (*b)->dump();
    }
    return true;
  }

private:
  ASTVisitorForHostInvastigator visitor;
};

class FrontendActionForHostInvastigator : public ASTFrontendAction {
public:
    FrontendActionForHostInvastigator(){}

    virtual std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &ci, 
        StringRef file) override {
            myRewriter.setSourceMgr(ci.getSourceManager(), ci.getLangOpts());
            return llvm::make_unique<ASTConsumerForHostInvastigator>(myRewriter);
       }

private:
    Rewriter myRewriter;
};

std::map<int, std::string> invastigateHostFile(ClangTool* tool){
    numKernelFiles = 0;
    tool->run(newFrontendActionFactory<FrontendActionForHostInvastigator>().get());
    return mapKernelFiles;
}
