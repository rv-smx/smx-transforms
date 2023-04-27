#include "llvm/Passes/PassPlugin.h"

#include "LoopStreamization.h"
#include "LoopTreePrinter.h"
#include "StreamMemoryAnalysis.h"

using namespace llvm;

PassPluginLibraryInfo getSMXTransformsPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "SMXTransforms", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            registerStreamMemoryAnalysis(PB);
            registerLoopTreePrinter(PB);
            registerLoopStreamization(PB);
          }};
}

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return getSMXTransformsPluginInfo();
}
