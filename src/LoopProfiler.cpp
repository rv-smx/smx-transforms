#include "LoopProfiler.h"

#include <cassert>
#include <queue>
#include <string>

#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

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

bool runOnFunction(Function &F, FunctionAnalysisManager &FAM,
                   FunctionCallee FuncEnter, FunctionCallee FuncExit) {
  bool Changed = false;
  auto &LI = FAM.getResult<LoopAnalysis>(F);
  SmallSet<BasicBlock *, 32> VisitedExits;

  std::queue<Loop *> Loops;
  for (auto Loop : LI)
    Loops.push(Loop);

  // Run on all loops.
  while (!Loops.empty()) {
    // Take the first loop from queue, push new loops to queue.
    auto Loop = Loops.front();
    Loops.pop();
    for (auto SubLoop : Loop->getSubLoops())
      Loops.push(SubLoop);

    auto Preheader = Loop->getLoopPreheader();
    if (!Preheader)
      continue;

    // Insert profile function to preheader.
    IRBuilder<> Builder(Preheader->getTerminator());
    auto EnterCall = Builder.CreateCall(FuncEnter);
    auto Loc = Loop->getStartLoc();
    EnterCall->setDebugLoc(Loc);

    SmallVector<BasicBlock *, 8> ExitBlocks;
    Loop->getExitBlocks(ExitBlocks);

    // Insert profile function to exit blocks.
    for (auto BB : ExitBlocks) {
      if (!VisitedExits.insert(BB).second)
        continue;

      if (auto LandingPad = BB->getLandingPadInst()) {
        Builder.SetInsertPoint(LandingPad->getNextNode());
      } else {
        Builder.SetInsertPoint(BB->getFirstNonPHI());
      }

      auto ExitCall = Builder.CreateCall(FuncExit);
      ExitCall->setDebugLoc(Loc);
    }

    // Mark as changed.
    Changed = true;
  }

  return Changed;
}

} // namespace

PreservedAnalyses LoopProfiler::run(Module &M,
                                    ModuleAnalysisManager &MAM) const {
  bool Changed = false;
  auto &Ctx = M.getContext();

  // Insert declarations of profile functions.
  auto ProfFuncTy = FunctionType::get(Type::getVoidTy(Ctx), false);
  auto ProfFuncEnter =
      M.getOrInsertFunction(LoopProfileFuncEnter.getValue(), ProfFuncTy);
  auto ProfFuncExit =
      M.getOrInsertFunction(LoopProfileFuncExit.getValue(), ProfFuncTy);

  // Set attributes for functions and their parameters.
  dyn_cast<Function>(ProfFuncEnter.getCallee())->setDoesNotThrow();
  dyn_cast<Function>(ProfFuncExit.getCallee())->setDoesNotThrow();

  // Run on all functions in the module.
  auto &FAM = MAM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();
  for (auto &F : M) {
    if (!F.isDeclaration() &&
        runOnFunction(F, FAM, ProfFuncEnter, ProfFuncExit))
      Changed = true;
  }

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

void registerLoopProfiler(PassBuilder &PB) {
  PB.registerPipelineParsingCallback(
      [](StringRef Name, ModulePassManager &MPM,
         ArrayRef<PassBuilder::PipelineElement>) {
        if (Name == "loop-profiler") {
          MPM.addPass(LoopProfiler());
          return true;
        }
        return false;
      });
}
