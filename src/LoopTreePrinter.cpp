#include "LoopTreePrinter.h"

#include <cstddef>

#include "llvm/Analysis/LoopInfo.h"

#include "Utils.h"

using namespace llvm;

namespace {

void scanLoops(raw_ostream &OS, bool &First, Loop *Current, Loop *Parent) {
  // Print comma.
  if (First) {
    First = false;
  } else {
    OS << ',';
  }

  // Print loop tree information.
  printDebugLoc(OS, Current->getStartLoc());
  OS << ":{\"parent\":";
  if (Parent) {
    printDebugLoc(OS, Parent->getStartLoc());
  } else {
    OS << "null";
  }
  OS << ",\"children\":[";
  const auto &SubLoops = Current->getSubLoops();
  for (std::size_t i = 0; i < SubLoops.size(); ++i) {
    if (i)
      OS << ',';
    printDebugLoc(OS, SubLoops[i]->getStartLoc());
  }

  // Scan for all sub loops.
  for (auto SubLoop : SubLoops)
    scanLoops(OS, First, SubLoop, Current);
}

} // namespace

PreservedAnalyses LoopTreePrinter::run(Function &F,
                                       FunctionAnalysisManager &FAM) const {
  auto &LI = FAM.getResult<LoopAnalysis>(F);

  // Scan for all loops.
  OS << '{';
  bool First = true;
  for (auto Loop : LI)
    scanLoops(OS, First, Loop, nullptr);
  OS << "}\n";

  return PreservedAnalyses::all();
}

void registerLoopTreePrinter(PassBuilder &PB) {
  PB.registerPipelineParsingCallback(
      [](StringRef Name, FunctionPassManager &FPM,
         ArrayRef<PassBuilder::PipelineElement>) {
        if (Name == "print<loop-trees>") {
          FPM.addPass(LoopTreePrinter(errs()));
          return true;
        }
        return false;
      });
}
