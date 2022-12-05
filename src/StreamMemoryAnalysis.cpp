#include "StreamMemoryAnalysis.h"

#include <cstddef>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"

using namespace llvm;

namespace {

void printBool(raw_ostream &OS, bool Bool) {
  if (Bool) {
    OS << "true";
  } else {
    OS << "false";
  }
}

void printString(raw_ostream &OS, StringRef Str) {
  OS << '"';
  OS.write_escaped(Str);
  OS << '"';
}

template <typename T> void printOptional(raw_ostream &OS, Optional<T> &Opt) {
  if (Opt) {
    Opt->print(OS);
  } else {
    OS << "null";
  }
}

template <typename T> void printArray(raw_ostream &OS, ArrayRef<T> Arr) {
  OS << '[';
  for (std::size_t i = 0; i < Arr.size(); ++i) {
    if (i)
      OS << ',';
    Arr[i].print(OS);
  }
  OS << ']';
}

void printLoop(raw_ostream &OS, Loop *Loop) {
  OS << "{name:";
  printString(Loop->getName());
  OS << ",annotatedParallel:";
  printBool(OS, Loop->isAnnotatedParallel());
  OS << ",depth:" << Loop->getLoopDepth();
  OS << ",innermost:";
  printBool(OS, Loop->isInnermost());
  OS << ",numBlocks:" << Loop->getNumBlocks() << '}';
}

} // namespace

void InductionVariableStream::IVValue::print(raw_ostream &OS) const {
  OS << "{isStatic:";
  printBool(OS, InitVal->IsStatic);
  OS << ",value:" << InitVal->Value << '}';
}

void InductionVariableStream::print(raw_ostream &OS) const {
  OS << "{name:";
  printString(OS, Name);
  OS << ",initVal:";
  printOptional(OS, InitVal);
  OS << ",finalVal:";
  printOptional(OS, FinalVal);
  OS << ",canonical:";
  printBool(OS, IsCanonical);
  OS << ",stepInstOpc:";
  printString(OS, Instruction::getOpcodeName(StepInstOpc));
  OS << '}';
}

void MemoryStream::AddressFactor::print(raw_ostream &OS) const {
  StringRef DepStreamStr, DepStreamTypeStr;
  switch (DepStreamType) {
  case InductionVariable:
    DepStreamStr = reinterpret_cast<InductionVariableStream *>(DepStream)->Name;
    DepStreamTypeStr = "inductionVariable";
    break;
  case Memory:
    DepStreamStr = reinterpret_cast<MemoryStream *>(DepStream)->Name;
    DepStreamTypeStr = "memory";
    break;
  case NotAStream:
    DepStreamStr = reinterpret_cast<Value *>(DepStream)->getName();
    DepStreamTypeStr = "notAStream";
    break;
  }
  OS << "{depStream:";
  printString(OS, DepStreamStr);
  OS << ",depStreamType:\"" << DepStreamTypeStr;
  OS << "\",stride:" << Stride << '}';
}

void MemoryStream::print(raw_ostream &OS) const {
  OS << "{name:";
  printString(OS, Name);
  OS << ",base:";
  printString(OS, Base->getName());
  OS << ",factors:";
  printArray(OS, Factors);
  OS << ",read:";
  printBool(OS, Read);
  OS << ",written:";
  printBool(OS, Written);
  OS << ",width:";
  printBool(OS, Width);
  OS << '}';
}

void MemoryOperation::print(raw_ostream &OS) const {
  OS << "{memOpc:";
  printString(Instruction::getOpcodeName(MemOpc));
  OS << ",memStream:";
  if (MemStream) {
    printString(OS, MemStream->Name);
  } else {
    OS << "null";
  }
  OS << '}';
}

void StreamInfo::print(raw_ostream &OS) const {
  OS << "{loop:";
  printLoop(OS, Loop);
  OS << ",ivs:";
  printArray(OS, IVs);
  OS << ",memStreams:";
  printArray(OS, MemStreams);
  OS << ",memOps:";
  printArray(OS, MemOps);
  OS << '}';
}

StreamMemoryAnalysis::Result
StreamMemoryAnalysis::run(Function &F, FunctionAnalysisManager &FAM) {
  llvm::SmallVector<StreamInfo, 0> SIs;
  auto &LI = FAM.getResult<LoopAnalysis>(F);
  for (auto Loop : LI) {
    // TODO
  }
  return SIs;
}

PreservedAnalyses
StreamMemoryAnalysisPrinter::run(Function &F, FunctionAnalysisManager &FAM) {
  auto &SIs = FAM.getResult<StreamMemoryAnalysis>(F);
  printArray(OS, SIs);
  return PreservedAnalyses::all();
}

void registerStreamMemoryAnalysis(PassBuilder &PB) {
  PB.registerPipelineParsingCallback(
      [](StringRef Name, FunctionPassManager &FPM,
         ArrayRef<PassBuilder::PipelineElement>) {
        if (Name == "print<stream-memory>") {
          FPM.addPass(StreamMemoryAnalysisPrinter(errs()));
          return true;
        }
        return false;
      });
  PB.registerAnalysisRegistrationCallback([](FunctionAnalysisManager &FAM) {
    FAM.registerPass([] { return StreamMemoryAnalysis(); })
  });
}
