#ifndef SMX_TRANSFORMS_SMX_PASS_H
#define SMX_TRANSFORMS_SMX_PASS_H

#include "llvm/IR/Function.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"

struct SMXPass : public llvm::PassInfoMixin<SMXPass> {
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &AM);
};

void registerSMXPass(llvm::PassBuilder &PB);

#endif // SMX_TRANSFORMS_SMX_PASS_H
