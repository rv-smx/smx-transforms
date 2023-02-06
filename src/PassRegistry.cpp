#include "llvm/Passes/PassPlugin.h"

#include "LoopProfiler.h"
#include "LoopTreePrinter.h"
#include "SMXPass.h"
#include "StreamMemoryAnalysis.h"

using namespace llvm;

PassPluginLibraryInfo getSMXTransformsPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "SMXTransforms", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            registerSMXPass(PB);
            registerStreamMemoryAnalysis(PB);
            registerLoopProfiler(PB);
            registerLoopTreePrinter(PB);
          }};
}

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return getSMXTransformsPluginInfo();
}
