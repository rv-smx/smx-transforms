#include "LoopStreamization.h"

using namespace llvm;

namespace {

//

} // namespace

PreservedAnalyses LoopStreamization::run(Function &F,
                                         FunctionAnalysisManager &FAM) const {
  bool Changed = false;

  // TODO

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

void registerLoopStreamization(llvm::PassBuilder &PB) {
  PB.registerPipelineParsingCallback(
      [](StringRef Name, ModulePassManager &MPM,
         ArrayRef<PassBuilder::PipelineElement>) {
        if (Name == "loop-streamization") {
          MPM.addPass(LoopStreamization());
          return true;
        }
        return false;
      });
}
