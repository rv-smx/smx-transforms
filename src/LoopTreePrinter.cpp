#include "LoopTreePrinter.h"

#include <cstddef>
#include <cstdlib>
#include <string>

#include "llvm/ADT/Optional.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Analysis/LoopInfo.h"

#include "Utils.h"

using namespace llvm;

namespace {

/// Set of loop IDs.
using LoopIdSet = SmallSet<Optional<std::string>, 1>;

/// Map for storing pairs of loop ID and the corresponding loop ID set.
using LoopParentMap = StringMap<LoopIdSet>;

/// Builds the loop parent map by the given loop.
void buildLoopParentMap(LoopParentMap &Pairs, const Loop *L) {
  // Gets loop ID of the current loop and its parent.
  auto Id = convPrintableToStr(L->getStartLoc());
  Optional<std::string> ParentId;
  if (auto Parent = L->getParentLoop()) {
    ParentId = convPrintableToStr(Parent->getStartLoc());
  }

  // Insert to the map.
  if (ParentId != Id)
    Pairs[Id].insert(ParentId);

  // Scan for all sub loops.
  for (auto SubLoop : L->getSubLoops())
    buildLoopParentMap(Pairs, SubLoop);
}

/// Prints loop IDs in the given loop ID set to the output stream.
void printLoopIds(raw_ostream &OS, const LoopIdSet &Ids) {
  OS << '[';
  bool First = true;
  for (const auto &Id : Ids) {
    if (First) {
      First = false;
    } else {
      OS << ',';
    }
    if (Id) {
      printString(OS, *Id);
    } else {
      OS << "null";
    }
  }
  OS << ']';
}

/// Prints pairs in the given loop parent map to the output stream.
void printLoopParentPairs(raw_ostream &OS, const LoopParentMap &Pairs) {
  OS << '{';
  bool First = true;
  for (const auto &It : Pairs) {
    if (First) {
      First = false;
    } else {
      OS << ',';
    }
    printString(OS, It.getKey());
    OS << ':';
    printLoopIds(OS, It.getValue());
  }
  OS << '}';
}

} // namespace

PreservedAnalyses LoopTreePrinter::run(Function &F,
                                       FunctionAnalysisManager &FAM) const {
  auto &LI = FAM.getResult<LoopAnalysis>(F);

  // Build loop parent map.
  LoopParentMap Pairs;
  for (auto Loop : LI)
    buildLoopParentMap(Pairs, Loop);

  // Print to the output stream.
  printLoopParentPairs(OS, Pairs);
  OS << '\n';

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
