#include "StreamMemoryAnalysis.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <queue>
#include <string>
#include <utility>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Analysis/IVDescriptors.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"

#include "Utils.h"

using namespace llvm;

namespace {

cl::opt<bool>
    AnalyseAllLoops("smx-analyse-all-loops",
                    cl::desc("Analyse all loops, not just the outermost ones."),
                    cl::init(false));

/// Scans loops and collects stream information.
class LoopScanner {
public:
  LoopScanner(ScalarEvolution &SE, const DataLayout &DL) : SE(SE), DL(DL) {}

  void runOnLoop(Loop *L) {
    SI.Loop = L;
    scanIVs(L);
    scanMemOps(L);
  }

  StreamInfo getStreamInfo() { return std::move(SI); }

private:
  /// Scans for induction variables.
  void scanIVs(Loop *L) {
    std::queue<std::pair<Loop *, InductionVariableStream *>> Loops;
    Loops.push({L, nullptr});
    while (!Loops.empty()) {
      auto [Loop, Parent] = Loops.front();
      Loops.pop();
      auto IV = collectIV(Loop, Parent);
      if (!IV)
        IV = Parent;
      for (auto SubLoop : Loop->getSubLoops()) {
        Loops.push({SubLoop, IV});
      }
    }
  }

  /// Scans for loads and stores.
  void scanMemOps(Loop *L) {
    for (auto BB : L->getBlocks()) {
      for (auto &I : *BB) {
        if (auto Load = dyn_cast<LoadInst>(&I)) {
          collectLoad(L, Load);
        } else if (auto Store = dyn_cast<StoreInst>(&I)) {
          collectStore(L, Store);
        }
      }
    }
  }

  /// Collects induction variable information of the given loop.
  InductionVariableStream *collectIV(Loop *L, InductionVariableStream *Parent) {
    // Make sure there is an induction variable in the loop.
    if (!L->isLoopSimplifyForm())
      return nullptr;
    PHINode *IV = nullptr;
    auto Header = L->getHeader();
    for (auto &PHI : Header->phis()) {
      InductionDescriptor IndDesc;
      if (InductionDescriptor::isInductionPHI(&PHI, L, &SE, IndDesc)) {
        IV = &PHI;
        break;
      }
    }
    if (!IV)
      return nullptr;
    // Fill the induction variable stream info.
    auto IVS = std::make_unique<InductionVariableStream>();
    IVS->Name = IV->getName();
    IVS->Parent = Parent;
    IVS->LoopDepth = L->getLoopDepth();
    IVS->IsCanonical = L->isCanonical(SE);
    if (auto Bounds = L->getBounds(SE)) {
      IVS->InitVal = makeIVValue(Bounds->getInitialIVValue());
      IVS->FinalVal = makeIVValue(Bounds->getFinalIVValue());
      IVS->IsIncreasing =
          Bounds->getDirection() == Loop::LoopBounds::Direction::Increasing;
      IVS->StepInstOpc = Bounds->getStepInst().getOpcode();
    }
    // Update the stream info.
    auto IVSPtr = IVS.get();
    IVs.insert({IV, IVSPtr});
    SI.IVs.push_back(std::move(IVS));
    return IVSPtr;
  }

  /// Collects the information of the given GEP.
  /// This method may return `nullptr`.
  MemoryStream *collectGEP(Loop *L, GetElementPtrInst *GEP) {
    // Skip if already collected.
    if (auto It = GEPs.find(GEP); It != GEPs.end())
      return It->second;
    // Skip if is a loop invariant.
    if (L->isLoopInvariant(GEP)) {
      GEPs.insert({GEP, nullptr});
      return nullptr;
    }
    // Create a new memory stream.
    auto MS = std::make_unique<MemoryStream>();
    MS->Name = GEP->getName();
    MS->ResultType = GEP->getResultElementType();
    MS->Width = DL.getTypeAllocSize(MS->ResultType).getFixedSize();
    GEPs.insert({GEP, MS.get()});
    // Initialize factors.
    auto ElemTy = GEP->getSourceElementType();
    for (unsigned Idx = 0; Idx < GEP->getNumOperands(); ++Idx) {
      auto Opr = GEP->getOperand(Idx);
      auto V = removeCast(Opr);
      void *DepStream = V;
      auto DepStreamKind = MemoryStream::AddressFactor::NotAStream;
      // Handle induction variable stream and memory stream.
      if (auto PHI = dyn_cast<PHINode>(V)) {
        if (auto It = IVs.find(PHI); It != IVs.end()) {
          DepStream = It->second;
          DepStreamKind = MemoryStream::AddressFactor::InductionVariable;
        }
      } else if (auto Load = dyn_cast<LoadInst>(V)) {
        if (auto MS = collectLoad(L, Load)->MemStream) {
          DepStream = MS;
          DepStreamKind = MemoryStream::AddressFactor::Memory;
        }
      }
      // Handle stride.
      unsigned Stride;
      if (Idx == 0) {
        // Base address, let the stride = 1.
        Stride = 1;
      } else if (Idx == 1) {
        Stride = DL.getTypeAllocSize(ElemTy).getFixedSize();
      } else {
        ElemTy = GetElementPtrInst::getTypeAtIndex(ElemTy, Opr);
        assert(ElemTy && "Invalid GEP element type!");
        Stride = DL.getTypeAllocSize(ElemTy).getFixedSize();
      }
      // Update factors.
      MS->Factors.push_back(
          {DepStream, DepStreamKind, Stride, L->isLoopInvariant(V)});
    }
    // Update the stream info.
    auto MSPtr = MS.get();
    SI.MemStreams.push_back(std::move(MS));
    return MSPtr;
  }

  /// Collects the information of the given load.
  MemoryOperation *collectLoad(Loop *L, LoadInst *Load) {
    // Skip if already collected.
    if (auto It = Loads.find(Load); It != Loads.end())
      return It->second;
    // Create a new memory operation.
    auto MO = std::make_unique<MemoryOperation>();
    MO->MemOpc = Load->getOpcode();
    Loads.insert({Load, MO.get()});
    // Check the pointer.
    auto Ptr = removeCast(Load->getPointerOperand());
    if (auto GEP = dyn_cast<GetElementPtrInst>(Ptr)) {
      MO->MemStream = collectGEP(L, GEP);
      if (MO->MemStream)
        MO->MemStream->Read = true;
    }
    // Update the stream info.
    auto MOPtr = MO.get();
    SI.MemOps.push_back(std::move(MO));
    return MOPtr;
  }

  /// Collects the information of the given store.
  void collectStore(Loop *L, StoreInst *Store) {
    auto MO = std::make_unique<MemoryOperation>();
    MO->MemOpc = Store->getOpcode();
    // Check the pointer.
    auto Ptr = removeCast(Store->getPointerOperand());
    if (auto GEP = dyn_cast<GetElementPtrInst>(Ptr)) {
      MO->MemStream = collectGEP(L, GEP);
      if (MO->MemStream)
        MO->MemStream->Written = true;
    }
    SI.MemOps.push_back(std::move(MO));
  }

  /// Creates a new `IVValue` by the given LLVM value.
  static InductionVariableStream::IVValue makeIVValue(Value &V) {
    auto C = dyn_cast<ConstantInt>(&V);
    return {V.getName(), !!C, C ? *C->getValue().getRawData() : 0};
  }

  /// Returns the source operand if the given value is a cast,
  /// otherwise returns the given value.
  static Value *removeCast(Value *V) {
    if (auto Cast = dyn_cast<CastInst>(V))
      return removeCast(Cast->getOperand(0));
    return V;
  }

  ScalarEvolution &SE;
  const DataLayout &DL;
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

template <typename T>
void printOptional(raw_ostream &OS, const Optional<T> &Opt) {
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
  OS << "{\"name\":";
  printString(OS, Loop->getName());
  OS << ",\"startLoc\":";
  printDebugLoc(OS, Loop->getStartLoc());
  OS << ",\"parent\":";
  if (auto Parent = Loop->getParentLoop()) {
    printString(OS, Parent->getName());
  } else {
    OS << "null";
  }
  OS << ",\"parentFunc\":";
  printString(OS, Loop->getHeader()->getParent()->getName());
  OS << ",\"annotatedParallel\":";
  printBool(OS, Loop->isAnnotatedParallel());
  OS << ",\"depth\":" << Loop->getLoopDepth();
  OS << ",\"numSubLoops\":" << Loop->getSubLoops().size();
  OS << ",\"numBlocks\":" << Loop->getNumBlocks() << '}';
}

} // namespace

AnalysisKey StreamMemoryAnalysis::Key;

void InductionVariableStream::IVValue::print(raw_ostream &OS) const {
  OS << "{\"name\":";
  printString(OS, Name);
  OS << ",\"constant\":";
  printBool(OS, IsConstant);
  OS << ",\"value\":" << Value << '}';
}

void InductionVariableStream::print(raw_ostream &OS) const {
  OS << "{\"name\":";
  printString(OS, Name);
  OS << ",\"parent\":";
  if (Parent) {
    printString(OS, Parent->Name);
  } else {
    OS << "null";
  }
  OS << ",\"loopDepth\":" << LoopDepth;
  OS << ",\"canonical\":";
  printBool(OS, IsCanonical);
  OS << ",\"initVal\":";
  printOptional(OS, InitVal);
  OS << ",\"finalVal\":";
  printOptional(OS, FinalVal);
  OS << ",\"increasing\":";
  printBool(OS, IsIncreasing);
  OS << ",\"stepInstOpc\":";
  if (StepInstOpc) {
    printString(OS, Instruction::getOpcodeName(*StepInstOpc));
  } else {
    OS << "null";
  }
  OS << '}';
}

void MemoryStream::AddressFactor::print(raw_ostream &OS) const {
  StringRef DepStreamStr, DepStreamKindStr;
  std::string ValueName;
  switch (DepStreamKind) {
  case InductionVariable:
    DepStreamStr = reinterpret_cast<InductionVariableStream *>(DepStream)->Name;
    DepStreamKindStr = "inductionVariable";
    break;
  case Memory:
    DepStreamStr = reinterpret_cast<MemoryStream *>(DepStream)->Name;
    DepStreamKindStr = "memory";
    break;
  case NotAStream: {
    auto V = reinterpret_cast<Value *>(DepStream);
    if (V->hasName()) {
      DepStreamStr = V->getName();
    } else {
      raw_string_ostream SS(ValueName);
      SS << V;
      SS.flush();
      DepStreamStr = ValueName;
    }
    DepStreamKindStr = "notAStream";
    break;
  }
  }
  OS << "{\"depStream\":";
  printString(OS, DepStreamStr);
  OS << ",\"depStreamKind\":\"" << DepStreamKindStr;
  OS << "\",\"stride\":" << Stride;
  OS << ",\"invariant\":";
  printBool(OS, IsInvariant);
  OS << '}';
}

void MemoryStream::print(raw_ostream &OS) const {
  OS << "{\"name\":";
  printString(OS, Name);
  OS << ",\"resultType\":";
  std::string ResultTypeStr;
  raw_string_ostream SS(ResultTypeStr);
  ResultType->print(SS);
  SS.flush();
  printString(OS, ResultTypeStr);
  OS << ",\"factors\":";
  printArray(OS, ArrayRef<AddressFactor>(Factors));
  OS << ",\"read\":";
  printBool(OS, Read);
  OS << ",\"written\":";
  printBool(OS, Written);
  OS << ",\"width\":" << Width << '}';
}

void MemoryOperation::print(raw_ostream &OS) const {
  OS << "{\"memOpcode\":";
  printString(OS, Instruction::getOpcodeName(MemOpc));
  OS << ",\"memStream\":";
  if (MemStream) {
    printString(OS, MemStream->Name);
  } else {
    OS << "null";
  }
  OS << '}';
}

void StreamInfo::print(raw_ostream &OS) const {
  OS << "{\"loop\":";
  printLoop(OS, Loop);
  OS << ",\"inductionVariableStreams\":";
  printPtrArray(OS, ArrayRef<std::unique_ptr<InductionVariableStream>>(IVs));
  OS << ",\"memStreams\":";
  printPtrArray(OS, ArrayRef<std::unique_ptr<MemoryStream>>(MemStreams));
  OS << ",\"memOps\":";
  printPtrArray(OS, ArrayRef<std::unique_ptr<MemoryOperation>>(MemOps));
  OS << '}';
}

StreamMemoryAnalysis::Result
StreamMemoryAnalysis::run(Function &F, FunctionAnalysisManager &FAM) const {
  Result Res;
  auto &LI = FAM.getResult<LoopAnalysis>(F);
  auto &SE = FAM.getResult<ScalarEvolutionAnalysis>(F);
  auto &DL = F.getParent()->getDataLayout();
  std::queue<Loop *> Loops;
  for (auto Loop : LI) {
    Loops.push(Loop);
  }
  while (!Loops.empty()) {
    auto Loop = Loops.front();
    Loops.pop();
    LoopScanner Scanner(SE, DL);
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
  printArray(OS, ArrayRef<StreamInfo>(SIs));
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
    FAM.registerPass([] { return StreamMemoryAnalysis(); });
  });
}
