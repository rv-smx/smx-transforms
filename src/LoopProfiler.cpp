#include "LoopProfiler.h"

#include <cassert>
#include <string>

#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Type.h"
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

bool runOnFunction(Function &F, FunctionAnalysisManager &FAM) {
  bool Changed = false;
  auto &LI = FAM.getResult<LoopAnalysis>(F);

  for (auto Loop : LI) {
    auto Preheader = Loop->getLoopPreheader();
    if (!Preheader)
      continue;

    // Insert profile function to preheader.
    // TODO

    SmallVector<BasicBlock *, 8> ExitBlocks;
    Loop->getExitBlocks(ExitBlocks);

    // Insert profile function to exit blocks.
    for (auto BB : ExitBlocks) {
      // TODO
    }
  }

  return Changed;
}

} // namespace

PreservedAnalyses LoopProfiler::run(Module &M,
                                    ModuleAnalysisManager &MAM) const {
  bool Changed = false;
  auto &Ctx = M.getContext();

  // Insert declarations of profile functions.
  auto StrTy = PointerType::getUnqual(Type::getInt8Ty(Ctx));
  auto ProfFuncTy =
      FunctionType::get(Type::getVoidTy(Ctx), {StrTy, StrTy}, false);
  auto ProfFuncEnter =
      M.getOrInsertFunction(LoopProfileFuncEnter.getValue(), ProfFuncTy);
  auto ProfFuncExit =
      M.getOrInsertFunction(LoopProfileFuncExit.getValue(), ProfFuncTy);

  // Set attributes for functions and their parameters.
  auto SetAttr = [](FunctionCallee F) {
    auto Func = dyn_cast<Function>(F.getCallee());
    Func->setDoesNotThrow();
    Func->addParamAttr(0, Attribute::NoCapture);
    Func->addParamAttr(0, Attribute::ReadOnly);
    Func->addParamAttr(1, Attribute::NoCapture);
    Func->addParamAttr(1, Attribute::ReadOnly);
  };
  SetAttr(ProfFuncEnter);
  SetAttr(ProfFuncExit);

  // Run on all functions in the module.
  auto &FAM = MAM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();
  for (auto &F : M) {
    if (!F.isDeclaration() && runOnFunction(F, FAM))
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
