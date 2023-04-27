#ifndef SMX_TRANSFORMS_LOOP_TREE_PRINTER_H
#define SMX_TRANSFORMS_LOOP_TREE_PRINTER_H

#include "llvm/IR/Function.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/raw_ostream.h"

class LoopTreePrinter : public llvm::PassInfoMixin<LoopTreePrinter> {
public:
  explicit LoopTreePrinter(llvm::raw_ostream &OS) : OS(OS) {}

  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &FAM) const;

private:
  llvm::raw_ostream &OS;
};

void registerLoopTreePrinter(llvm::PassBuilder &PB);

#endif // SMX_TRANSFORMS_LOOP_TREE_PRINTER_H
