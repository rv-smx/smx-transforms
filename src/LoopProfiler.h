#ifndef SMX_TRANSFORMS_LOOP_PROFILER_H
#define SMX_TRANSFORMS_LOOP_PROFILER_H

#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"

struct LoopProfiler : public llvm::PassInfoMixin<LoopProfiler> {
  llvm::PreservedAnalyses run(llvm::Module &M,
                              llvm::ModuleAnalysisManager &MAM) const;
};

void registerLoopProfiler(llvm::PassBuilder &PB);

#endif // SMX_TRANSFORMS_LOOP_PROFILER_H
