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
      auto IV = collectIV(L, Loop, Parent);
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
  InductionVariableStream *collectIV(Loop *Top, Loop *L,
                                     InductionVariableStream *Parent) {
    // Make sure there is an induction variable in the loop.
    if (!L->isLoopSimplifyForm())
      return nullptr;
    PHINode *IV = nullptr;
    InductionDescriptor IndDesc;
    auto Header = L->getHeader();
    for (auto &PHI : Header->phis()) {
      if (InductionDescriptor::isInductionPHI(&PHI, L, &SE, IndDesc)) {
        IV = &PHI;
        break;
      }
    }
    if (!IV)
      return nullptr;
    // Fill the induction variable stream info.
    auto IVS = std::make_unique<InductionVariableStream>();
    IVS->Parent = Parent;
    IVS->Loop = L;
    IVS->IsCanonical = L->isCanonical(SE);
    // Fill value info.
    IVS->InitVal = IndDesc.getStartValue();
    IVS->StepVal = IndDesc.getStep();
    auto StepInst = IV->getIncomingValueForBlock(L->getLoopLatch());
    IVS->FinalVal = findFinalValue(Top, L, IV, StepInst);
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
    MS->GEP = GEP;
    MS->Width = DL.getTypeAllocSize(GEP->getResultElementType()).getFixedSize();
    GEPs.insert({GEP, MS.get()});
    // Initialize factors.
    auto ElemTy = GEP->getSourceElementType();
    for (unsigned Idx = 0; Idx < GEP->getNumOperands(); ++Idx) {
      auto Opr = GEP->getOperand(Idx);
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
      // Update address factors.
      updateAddrFactors(MS.get(), L, Opr, Stride);
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

  /// Updates address factor information for the given memory stream.
  void updateAddrFactors(MemoryStream *MS, Loop *L, Value *Opr,
                         unsigned Stride) {
    auto Num = collectAddrFactors(MS, L, Opr, false);
    for (auto Idx = MS->Factors.size() - Num; Idx < MS->Factors.size(); ++Idx) {
      MS->Factors[Idx].Strides.push_back(
          {Stride, MemoryStream::Stride::Mul, true});
    }
  }

  /// Collects address factors for the given memory stream.
  /// Returns the number of added address factors.
  size_t collectAddrFactors(MemoryStream *MS, Loop *L, Value *Opr, bool IsNeg) {
    auto V = removeCast(Opr);
    bool IsInvariant = L->isLoopInvariant(V);
    if (auto PHI = dyn_cast<PHINode>(V)) {
      if (auto It = IVs.find(PHI); It != IVs.end()) {
        // Induction variable stream.
        MS->Factors.push_back({It->second, {}, IsInvariant, IsNeg});
        return 1;
      }
    } else if (auto Load = dyn_cast<LoadInst>(V)) {
      if (auto M = collectLoad(L, Load)->MemStream) {
        // Memory stream.
        MS->Factors.push_back({M, {}, IsInvariant, IsNeg});
        return 1;
      }
    } else if (auto Bin = dyn_cast<BinaryOperator>(V)) {
      // Handle add/sub/mul/shl/sdiv/udiv.
      switch (Bin->getOpcode()) {
      case Instruction::BinaryOps::Add:
      case Instruction::BinaryOps::Sub: {
        bool IsRhsNeg = IsNeg;
        if (Bin->getOpcode() == Instruction::BinaryOps::Sub)
          IsRhsNeg = !IsRhsNeg;
        return collectAddrFactors(MS, L, Bin->getOperand(0), IsNeg) +
               collectAddrFactors(MS, L, Bin->getOperand(1), IsRhsNeg);
      }
      case Instruction::BinaryOps::Mul: {
        auto [Lhs, Rhs] = getMulOperands(Bin);
        auto Num = collectAddrFactors(MS, L, Lhs, IsNeg);
        for (auto Idx = MS->Factors.size() - Num; Idx < MS->Factors.size();
             ++Idx) {
          MS->Factors[Idx].Strides.push_back(
              {Rhs, MemoryStream::Stride::Mul, L->isLoopInvariant(Rhs)});
        }
        return Num;
      }
      case Instruction::BinaryOps::Shl:
      case Instruction::BinaryOps::SDiv:
      case Instruction::BinaryOps::UDiv: {
        auto Num = collectAddrFactors(MS, L, Bin->getOperand(0), IsNeg);
        auto Op = Bin->getOpcode() == Instruction::BinaryOps::Shl
                      ? MemoryStream::Stride::Shl
                  : Bin->getOpcode() == Instruction::BinaryOps::SDiv
                      ? MemoryStream::Stride::SDiv
                      : MemoryStream::Stride::UDiv;
        for (auto Idx = MS->Factors.size() - Num; Idx < MS->Factors.size();
             ++Idx) {
          MS->Factors[Idx].Strides.push_back(
              {Bin->getOperand(1), Op, L->isLoopInvariant(Bin->getOperand(1))});
        }
        return Num;
      }
      default:;
      }
    }
    // Not a stream.
    MS->Factors.push_back({V, {}, IsInvariant, IsNeg});
    return 1;
  }

  /// Returns the final value of the loop induction variable if found.
  static Optional<InductionVariableStream::FinalValue>
  findFinalValue(Loop *Top, Loop *L, PHINode *IV, Value *StepInst) {
    auto LatchCmpInst = L->getLatchCmpInst();
    if (!LatchCmpInst)
      return None;

    auto Op0 = LatchCmpInst->getOperand(0);
    auto Op1 = LatchCmpInst->getOperand(1);
    Value *V = nullptr;
    if (Op0 == IV || Op0 == StepInst)
      V = Op1;
    if (Op1 == IV || Op1 == StepInst)
      V = Op0;
    if (!V)
      return None;

    return InductionVariableStream::FinalValue{V, Top->isLoopInvariant(V),
                                               LatchCmpInst->getPredicate()};
  }

  /// Returns the source operand if the given value is a cast,
  /// otherwise returns the given value.
  static Value *removeCast(Value *V) {
    if (auto Cast = dyn_cast<CastInst>(V))
      return removeCast(Cast->getOperand(0));
    return V;
  }

  /// Checks operands of a multiply instruction and returns them.
  static std::pair<Value *, Value *> getMulOperands(BinaryOperator *Bin) {
    auto Lhs = removeCast(Bin->getOperand(0));
    auto Rhs = removeCast(Bin->getOperand(1));
    if (isa<PHINode>(Lhs) || isa<LoadInst>(Lhs) ||
        (isa<BinaryOperator>(Lhs) && !isa<BinaryOperator>(Rhs)))
      return {Lhs, Rhs};
    if (isa<PHINode>(Rhs) || isa<LoadInst>(Rhs) ||
        (isa<BinaryOperator>(Rhs) && !isa<BinaryOperator>(Lhs)))
      return {Rhs, Lhs};
    return {Lhs, Rhs};
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
  printPrintable(OS, Loop->getStartLoc());
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

void printValue(raw_ostream &OS, Value *Value) {
  if (Value->hasName()) {
    printString(OS, Value->getName());
  } else {
    std::string Name;
    raw_string_ostream SS(Name);
    SS << Value;
    SS.flush();
    printString(OS, Name);
  }
}

} // namespace

AnalysisKey StreamMemoryAnalysis::Key;

void InductionVariableStream::FinalValue::print(raw_ostream &OS) const {
  OS << "{\"value\":";
  printValue(OS, Value);
  OS << ",\"invariant\":";
  printBool(OS, IsInvariant);
  OS << ",\"cond\":";
  printString(OS, CmpInst::getPredicateName(Cond));
  OS << '}';
}

void InductionVariableStream::print(raw_ostream &OS) const {
  OS << "{\"name\":";
  printString(OS, PHI->getName());
  OS << ",\"parent\":";
  if (Parent) {
    printString(OS, Parent->PHI->getName());
  } else {
    OS << "null";
  }
  OS << ",\"loopDepth\":" << Loop->getLoopDepth();
  OS << ",\"loopStartLoc\":";
  printPrintable(OS, Loop->getStartLoc());
  OS << ",\"canonical\":";
  printBool(OS, IsCanonical);
  OS << ",\"initVal\":";
  printValue(OS, InitVal);
  OS << ",\"stepVal\":";
  printPrintable(OS, *StepVal);
  OS << ",\"finalVal\":";
  printOptional(OS, FinalVal);
  OS << '}';
}

void MemoryStream::Stride::print(raw_ostream &OS) const {
  StringRef ValueKindStr;
  OS << "{\"value\":";
  if (auto ConstInt = std::get_if<unsigned>(&Value)) {
    OS << *ConstInt;
    ValueKindStr = "constInt";
  } else {
    auto V = std::get_if<::Value *>(&Value);
    assert(V && "WTF?");
    printValue(OS, *V);
    ValueKindStr = "value";
  }
  OS << ",\"valueKind\":\"" << ValueKindStr;
  OS << "\",\"op\":\"";
  switch (Op) {
  case Mul:
    OS << "mul";
    break;
  case Shl:
    OS << "shl";
    break;
  case SDiv:
    OS << "sdiv";
    break;
  case UDiv:
    OS << "udiv";
    break;
  }
  OS << "\",\"invariant\":";
  printBool(OS, IsInvariant);
  OS << '}';
}

void MemoryStream::AddressFactor::print(raw_ostream &OS) const {
  StringRef DepStreamKindStr;
  OS << "{\"depStream\":";
  if (auto IV = std::get_if<InductionVariableStream *>(&DepStream)) {
    printString(OS, (*IV)->PHI->getName());
    DepStreamKindStr = "inductionVariable";
  } else if (auto MS = std::get_if<MemoryStream *>(&DepStream)) {
    printString(OS, (*MS)->GEP->getName());
    DepStreamKindStr = "memory";
  } else {
    auto V = std::get_if<Value *>(&DepStream);
    assert(V && "WTF?");
    printValue(OS, *V);
    DepStreamKindStr = "notAStream";
  }
  OS << ",\"depStreamKind\":\"" << DepStreamKindStr;
  OS << "\",\"strides\":";
  printArray(OS, ArrayRef<Stride>(Strides));
  OS << ",\"invariant\":";
  printBool(OS, IsInvariant);
  OS << ",\"neg\":";
  printBool(OS, IsNeg);
  OS << '}';
}

void MemoryStream::print(raw_ostream &OS) const {
  OS << "{\"name\":";
  printString(OS, GEP->getName());
  OS << ",\"resultType\":";
  printPrintable(OS, *GEP->getResultElementType());
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
    printString(OS, MemStream->GEP->getName());
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
