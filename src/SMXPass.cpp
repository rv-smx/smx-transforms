#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace {

struct SMXPass : public PassInfoMixin<SMXPass> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM) {
    errs() << "Hello: " << F.getName() << '\n';
    return PreservedAnalyses::all();
  }
};

} // namespace

PassPluginLibraryInfo getSMXPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "SMXPass", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, FunctionPassManager &FPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "smx") {
                    FPM.addPass(SMXPass());
                    return true;
                  }
                  return false;
                });
          }};
}

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return getSMXPassPluginInfo();
}
