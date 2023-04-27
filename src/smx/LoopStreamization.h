#ifndef SMX_TRANSFORMS_LOOP_STREAMIZATION_H
#define SMX_TRANSFORMS_LOOP_STREAMIZATION_H

#include "llvm/IR/Function.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"

struct LoopStreamization : public llvm::PassInfoMixin<LoopStreamization> {
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &FAM) const;
};

void registerLoopStreamization(llvm::PassBuilder &PB);

#endif // SMX_TRANSFORMS_LOOP_STREAMIZATION_H
