#include "LoopProfiler.h"

#include <string>

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;

namespace {

cl::opt<std::string> LoopProfileFuncEnter(
    "loop-profile-func-enter",
    cl::desc("Name of the function to be called when entering a loop."),
    cl::init("__loop_profile_func_enter"));

cl::opt<std::string> LoopProfileFuncExit(
    "loop-profile-func-exit",
    cl::desc("Name of the function to be called when exiting a loop."),
    cl::init("__loop_profile_func_exit"));

} // namespace

PreservedAnalyses LoopProfiler::run(Function &F,
                                    FunctionAnalysisManager &FAM) const {
  auto &LI = FAM.getResult<LoopAnalysis>(F);

  bool Changed = false;

  // TODO

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

void registerLoopProfiler(PassBuilder &PB) {
  PB.registerPipelineParsingCallback(
      [](StringRef Name, FunctionPassManager &FPM,
         ArrayRef<PassBuilder::PipelineElement>) {
        if (Name == "loop-profiler") {
          FPM.addPass(LoopProfiler());
          return true;
        }
        return false;
      });
}
