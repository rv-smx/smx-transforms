#include "SMXPass.h"
#include "StreamMemoryAnalysis.h"

#include "llvm/Passes/PassPlugin.h"

using namespace llvm;

PassPluginLibraryInfo getSMXTransformsPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "SMXTransforms", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            registerSMXPass(PB);
            registerStreamMemoryAnalysis(PB);
          }};
}

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return getSMXTransformsPluginInfo();
}
