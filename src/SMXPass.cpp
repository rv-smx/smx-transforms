#include "SMXPass.h"

#include "llvm/Support/raw_ostream.h"

using namespace llvm;

PreservedAnalyses SMXPass::run(Function &F, FunctionAnalysisManager &AM) {
  errs() << "Hello: " << F.getName() << '\n';
  return PreservedAnalyses::all();
}

void registerSMXPass(PassBuilder &PB) {
  PB.registerPipelineParsingCallback(
      [](StringRef Name, FunctionPassManager &FPM,
         ArrayRef<PassBuilder::PipelineElement>) {
        if (Name == "smx") {
          FPM.addPass(SMXPass());
          return true;
        }
        return false;
      });
}
