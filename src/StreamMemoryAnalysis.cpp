#include "StreamMemoryAnalysis.h"

#include <algorithm>
#include <cstddef>
#include <queue>
#include <string>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;

namespace {

cl::opt<bool>
    AnalyseAllLoops("smx-analyse-all-loops",
                    cl::desc("Analyse all loops, not just the outermost ones."),
                    cl::init(false));

/// Scans loops and collects stream information.
class LoopScanner {
public:
  LoopScanner(ScalarEvolution &SE) : SE(SE) {}

  void runOnLoop(Loop *L) {
    SI.Loop = L;
    scanIVs(L);
    scanGEPsAndMemOps(L);
  }

  StreamInfo getStreamInfo() const { return SI; }

private:
  /// Scans for induction variables.
  void scanIVs(Loop *L) {
    std::queue<Loop *> Loops;
    Loops.push(L);
    while (!Loops.empty()) {
      auto Loop = Loops.front();
      Loops.pop();
      collectIV(Loop);
      for (auto SubLoop : Loop->getSubLoops()) {
        Loops.push(SubLoop);
      }
    }
  }

  /// Scans for GEPs, loads and stores.
  void scanGEPsAndMemOps(Loop *L) {
    for (auto BB : L->getBlocks()) {
      for (const auto &I : *BB) {
        if (auto GEP = dyn_cast<GetElementPtrInst>(I)) {
          collectGEP(L, GEP);
        } else if (auto Load = dyn_cast<LoadInst>(I)) {
          collectLoad(Load);
        } else if (auto Store = dyn_cast<StoreInst>(I)) {
          collectStore(Store);
        }
      }
    }
  }

  /// Collects induction variable information of the given loop.
  void collectIV(Loop *L) {
    // Make sure there is an induction variable in the loop.
    auto IV = L->getInductionVariable(SE);
    if (!IV)
      return;
    // Fill the induction variable stream info.
    auto IVS = std::make_unique<InductionVariableStream>();
    IVS->Name = IV->getName();
    IVS->LoopDepth = L->getLoopDepth();
    IVS->IsCanonical = L->isCanonical();
    if (auto Bounds = L->getBounds(SE)) {
      IVS->InitVal = makeIVValue(Bounds->getInitialIVValue());
      IVS->FinalVal = makeIVValue(Bounds->getFinalIVValue());
      IVS->IsIncreasing =
          Bounds->getDirection() == Loop::LoopBounds::Direction::Increasing;
      IVS->StepInstOpc = Bounds->getStepInst().getOpcode();
    }
    // Update the stream info.
    IVs.insert({IV, IVS.get()});
    SI.IVs.push_back(std::move(IVS));
  }

  /// Collects the information of the given GEP.
  /// This method may return `nullptr`.
  MemoryStream *collectGEP(Loop *L, GetElementPtrInst *GEP) {
    // Skip if already collected.
    auto It = GEPs.find(GEP);
    if (It != GEPs.end())
      return It->second;
    // Check if the GEP can be a memory stream.
    auto Base = GEP->getPointerOperand();
    bool IsValidStream = !GEP->getResultElementType()->isVectorTy() &&
                         !L->isLoopInvariant(GEP) && L->isLoopInvariant(Base);
    // Collect operands.
    SmallVector<Value *> Indices;
    if (IsValidStream) {
      IsValidStream = false;
      for (const auto &U : GEP->indices()) {
        auto Index = removeCast(U.get());
        if (auto PHI = dyn_cast<PHINode>(Index)) {
          if (IVs.count(PHI))
            IsValidStream = true;
        }
        Indices.push_back(Index);
      }
    }
    // Skip if the GEP is not a valid stream.
    if (!IsValidStream) {
      GEPs.insert({GEP, nullptr});
      return nullptr;
    }
    // Create a new memory stream.
    auto MS = std::make_unique<MemoryStream>();
    MS->Name = GEP->getName();
    MS->Base = Base;
    MS->Width = GEP->getResultElementType()->getScalarSizeInBits();
    GEPs.insert({GEP, MS.get()});
    // Initialize factors.
    for (auto V : Indices) {
      void *DepStream = V;
      unsigned DepStreamKind = MemoryStream::AddressFactor::NotAStream;
      if (auto PHI = dyn_cast<PHINode>(V)) {
        auto It = IVs.find(PHI);
        if (It != IVs.end()) {
          DepStream = It->second;
          DepStreamKind = MemoryStream::AddressFactor::InductionVariable;
        }
      } else if (auto Load = dyn_cast<LoadInst>(V)) {
        if (auto MS = collectLoad(Load)->MemStream) {
          DepStream = MS;
          DepStreamKind = MemoryStream::AddressFactor::Memory;
        }
      }
      // TODO: stride
    }
    // Update the stream info.
    auto MSPtr = MS.get();
    SI.MemStreams.push_back(std::move(MS));
    return MSPtr;
  }

  /// Collects the information of the given load.
  MemoryOperation *collectLoad(LoadInst *Load) {
    // TODO
  }

  /// Collects the information of the given store.
  void collectStore(StoreInst *Store) {
    // TODO
  }

  /// Creates a new `IVValue` by the given LLVM value.
  static InductionVariableStream::IVValue makeIVValue(Value &V) {
    auto C = dyn_cast<ConstantInt>(&V);
    return {!!C, C ? *C->getValue().getRawData() : 0};
  }

  /// Returns the source operand if the given value is a cast,
  /// otherwise returns the given value.
  static Value *removeCast(Value *V) {
    if (auto Cast = dyn_cast<CastInst>(V))
      return removeCast(Cast->getOperand(0));
    return V;
  }

  ScalarEvolution &SE;
  StreamInfo SI;
  DenseMap<PHINode *, InductionVariableStream *> IVs;
  DenseMap<GetElementPtrInst *, MemoryStream *> GEPs;
  DenseMap<LoadInst *, MemoryOperation *> Loads;
};

void printBool(raw_ostream &OS, bool Bool) {
  if (Bool) {
    OS << "true";
  } else {
    OS << "false";
  }
}

void printString(raw_ostream &OS, StringRef Str) {
  OS << '"';
  for (auto C : Str) {
    switch (C) {
    case '\\':
      OS << "\\\\";
      break;
    case '\t':
      OS << "\\t";
      break;
    case '\n':
      OS << "\\n";
      break;
    case '"':
      OS << "\\\"";
      break;
    default:
      if (isPrint(C)) {
        OS << C;
      } else {
        OS << "\\u00";
        OS << hexdigit((C >> 4) & 0xF);
        OS << hexdigit((C >> 0) & 0xF);
      }
    }
  }
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

template <typename T> void printPtrArray(raw_ostream &OS, ArrayRef<T> Arr) {
  OS << '[';
  for (std::size_t i = 0; i < Arr.size(); ++i) {
    if (i)
      OS << ',';
    Arr[i]->print(OS);
  }
  OS << ']';
}

void printLoop(raw_ostream &OS, Loop *Loop) {
  OS << "{name:";
  printString(Loop->getName());
  OS << ",startLoc:";
  std::string Loc;
  raw_string_ostream SS(Loc);
  Loop->getStartLoc().print(SS);
  SS.flush();
  printString(OS, Loc);
  OS << ",parent:";
  if (auto Parent = Loop->getParentLoop()) {
    printString(OS, Parent->getName());
  } else {
    OS << "null";
  }
  OS << ",parentFunc:";
  printString(OS, Loop->getHeader()->getParent()->getName());
  OS << ",annotatedParallel:";
  printBool(OS, Loop->isAnnotatedParallel());
  OS << ",depth:" << Loop->getLoopDepth();
  OS << ",numSubLoops:" << Loop->getSubLoops().size();
  OS << ",numBlocks:" << Loop->getNumBlocks() << '}';
}

} // namespace

void InductionVariableStream::IVValue::print(raw_ostream &OS) const {
  OS << "{constant:";
  printBool(OS, InitVal->IsConstant);
  OS << ",value:" << InitVal->Value << '}';
}

void InductionVariableStream::print(raw_ostream &OS) const {
  OS << "{name:";
  printString(OS, Name);
  OS << ",loopDepth:" << LoopDepth;
  OS << ",canonical:";
  printBool(OS, IsCanonical);
  OS << ",initVal:";
  printOptional(OS, InitVal);
  OS << ",finalVal:";
  printOptional(OS, FinalVal);
  OS << ",increasing:";
  printBool(OS, IsIncreasing);
  OS << ",stepInstOpc:";
  if (StepInstOpc) {
    printString(OS, Instruction::getOpcodeName(*StepInstOpc));
  } else {
    OS << "null";
  }
  OS << '}';
}

void MemoryStream::AddressFactor::print(raw_ostream &OS) const {
  StringRef DepStreamStr, DepStreamKindStr;
  switch (DepStreamKind) {
  case InductionVariable:
    DepStreamStr = reinterpret_cast<InductionVariableStream *>(DepStream)->Name;
    DepStreamKindStr = "inductionVariable";
    break;
  case Memory:
    DepStreamStr = reinterpret_cast<MemoryStream *>(DepStream)->Name;
    DepStreamKindStr = "memory";
    break;
  case NotAStream:
    DepStreamStr = reinterpret_cast<Value *>(DepStream)->getName();
    DepStreamKindStr = "notAStream";
    break;
  }
  OS << "{depStream:";
  printString(OS, DepStreamStr);
  OS << ",depStreamKind:\"" << DepStreamKindStr;
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
  printPtrArray(OS, IVs);
  OS << ",memStreams:";
  printPtrArray(OS, MemStreams);
  OS << ",memOps:";
  printArray(OS, MemOps);
  OS << '}';
}

StreamMemoryAnalysis::Result
StreamMemoryAnalysis::run(Function &F, FunctionAnalysisManager &FAM) const {
  Result Res;
  auto &LI = FAM.getResult<LoopAnalysis>(F);
  auto &SE = FAM.getResult<ScalarEvolutionAnalysis>(F);
  std::queue<Loop *> Loops;
  for (auto Loop : LI) {
    Loops.push(Loop);
  }
  while (!Loops.empty()) {
    auto Loop = Loops.front();
    Loops.pop();
    LoopScanner Scanner(SE);
    Scanner.runOnLoop(Loop);
    Res.push_back(Scanner.getStreamInfo());
    if (AnalyseAllLoops) {
      for (auto SubLoop : Loop->getSubLoops()) {
        Loops.push(SubLoop);
      }
    }
  }
  return Res;
}

PreservedAnalyses
StreamMemoryAnalysisPrinter::run(Function &F,
                                 FunctionAnalysisManager &FAM) const {
  auto &SIs = FAM.getResult<StreamMemoryAnalysis>(F);
  printArray(OS, SIs);
  OS << '\n';
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
