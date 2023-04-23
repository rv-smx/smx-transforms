#include "LoopStreamization.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <queue>
#include <string>

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicsRISCV.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/ScalarEvolutionExpander.h"

#include "StreamMemoryAnalysis.h"

using namespace llvm;

namespace {

/// @note Order matters!
enum SelectStrategyTy {
  All,
  AllOuter,
  AllInner,
  Marked,
  MarkedOuter,
  MarkedInner,
};

cl::opt<SelectStrategyTy> SelectStrategy(
    "smx-sel-strategy", cl::desc("Loop chain selection strategy."),
    cl::values(
        clEnumValN(All, "all", "Select all loops in chain."),
        clEnumValN(AllOuter, "all-outer",
                   "Select all, keep outer loops if chain is too long."),
        clEnumValN(AllInner, "all-inner",
                   "Select all, keep inner loops if chain is too long."),
        clEnumValN(Marked, "marked", "Select only marked loops in chain."),
        clEnumValN(MarkedOuter, "marked-outer",
                   "Select marked, keep outer loops if chain is too long."),
        clEnumValN(MarkedInner, "marked-inner",
                   "Select marked, keep inner loops if chain is too long.")),
    cl::init(All));

cl::opt<unsigned> MaxIndvarStreams(
    "smx-max-ivs",
    cl::desc("Maximum number of supported induction variable streams."),
    cl::init(4));

cl::opt<bool>
    TruncateMems("smx-trunc-mems",
                 cl::desc("Truncate if there are too many memory streams."),
                 cl::init(false));

cl::opt<unsigned>
    MaxMemoryStreams("smx-max-mss",
                     cl::desc("Maximum number of supported memory streams."),
                     cl::init(32));

cl::opt<unsigned> MaxDataWidth(
    "smx-max-width",
    cl::desc("Maximum data width in bytes of memory stream accessing."),
    cl::init(128));

cl::opt<unsigned> MaxAddrFactors(
    "smx-max-afs",
    cl::desc("Maximum number of supported address factors per memory stream."),
    cl::init(4));

cl::opt<bool> NoIndirect("smx-no-indirect",
                         cl::desc("Disable indirect memory stream access."),
                         cl::init(false));

cl::opt<bool>
    ConfigOnly("smx-config-only",
               cl::desc("Only insert configurations, do not enable DAE."),
               cl::init(false));

cl::opt<bool> AllowNoMemoryStreams(
    "smx-allow-no-mss",
    cl::desc("Allow that there are no memory streams in loop."),
    cl::init(false));

/// Checks if the given loop ID said the loop should be streamized.
bool shouldBeStreamized(const MDNode *LoopID) {
  assert(LoopID->getNumOperands() > 0 && "requires at least one operand");
  assert(LoopID->getOperand(0) == LoopID && "invalid loop id");

  for (unsigned i = 1, e = LoopID->getNumOperands(); i < e; ++i) {
    MDNode *MD = dyn_cast<MDNode>(LoopID->getOperand(i));
    if (!MD)
      continue;

    MDString *S = dyn_cast<MDString>(MD->getOperand(0));
    if (!S)
      continue;

    if (S->getString() == "llvm.loop.smx.streamize.enable")
      return true;
  }
  return false;
}

/// Emits a warning message for the given loop.
raw_ostream &emitWarning(const Loop *L) {
  errs() << "\033[1m\033[33mLoopStreamization WARNING\033[0m: loop: ";
  L->getStartLoc().print(errs());
  errs() << ": ";
  return errs();
}

/// Converts LLVM comparison instruction predicate to SMX stop condition.
unsigned predToStopCond(CmpInst::Predicate Pred) {
  switch (Pred) {
  case CmpInst::ICMP_EQ:
    return 8;
  case CmpInst::ICMP_NE:
    return 9;
  case CmpInst::ICMP_UGT:
    return 4;
  case CmpInst::ICMP_UGE:
    return 5;
  case CmpInst::ICMP_ULT:
    return 6;
  case CmpInst::ICMP_ULE:
    return 7;
  case CmpInst::ICMP_SGT:
    return 0;
  case CmpInst::ICMP_SGE:
    return 1;
  case CmpInst::ICMP_SLT:
    return 2;
  case CmpInst::ICMP_SLE:
    return 3;
  default:
    llvm_unreachable("Invalid predicate");
  }
}

/// Helper for per-loop stream information.
class LoopStreamInfo {
public:
  LoopStreamInfo(const StreamMemoryAnalysis::Result &SIs) {
    for (const auto &SI : SIs) {
      for (const auto &IV : SI.IVs) {
        auto [_, Succ] = IVs.insert({IV->Loop, IV.get()});
        static_cast<void>(Succ);
        assert(Succ && "Duplicated induction variable stream!");
      }
      for (const auto &MS : SI.MemStreams) {
        auto [_, Succ] = MSs.insert({MS->GEP, MS.get()});
        static_cast<void>(Succ);
        assert(Succ && "Duplicated memory stream!");
      }
    }
  }

  /// Gets the induction variable stream information of the given loop.
  /// Returns `nullptr` if not found.
  const InductionVariableStream *getIndvarStream(const Loop *L) const {
    auto It = IVs.find(L);
    if (It == IVs.end())
      return nullptr;
    return It->second;
  }

  /// Gets the memory stream information of the given GEP.
  /// Returns `nullptr` if not found.
  const MemoryStream *getMemoryStream(const GetElementPtrInst *GEP) const {
    auto It = MSs.find(GEP);
    if (It == MSs.end())
      return nullptr;
    return It->second;
  }

private:
  DenseMap<const Loop *, const InductionVariableStream *> IVs;
  DenseMap<const GetElementPtrInst *, const MemoryStream *> MSs;
};

/// Helper for streamize loops.
class Streamizer {
public:
  Streamizer(Function &F, const SmallSet<const Loop *, 4> &Hints,
             ScalarEvolution &SE, const StreamMemoryAnalysis::Result &SIs)
      : F(F), Hints(Hints), SE(SE), LSI(SIs) {
    SizeTy = Type::getIntNTy(F.getContext(),
                             SE.getDataLayout().getPointerSizeInBits());
  }

  /// Tries to streamize the given loop, returns `true` if IR changed.
  bool tryOn(const Loop *L) {
    // Determine the loop chain to streamize.
    auto Loops = findPath(L);
    if (Loops.empty()) {
      emitWarning(L) << "Can not determine a loop chain to streamize!\n";
      return false;
    }

    // Check if the selected loops can be streamized.
    IVVec IVs;
    MSVec MSs;
    if (!canBeStreamized(Loops, IVs, MSs)) {
      emitWarning(L) << "Can not streamize this loop!\n";
      return false;
    }
    if (MSs.empty()) {
      emitWarning(L) << "No valid memory streams detected!\n";
      if (!AllowNoMemoryStreams)
        return false;
    }

    // Perform streamization.
    auto Outermost = Loops.front();
    auto Preheader = Outermost->getLoopPreheader();
    assert(Preheader && "The outermost loop has no preheader!");
    insertStreamConfigs(Outermost, *Preheader, IVs, MSs);
    if (!ConfigOnly) {
      replaceIndvarUpdates(IVs);
      replaceMemoryAccesses(MSs);
    }
    SmallVector<BasicBlock *, 8> ExitBlocks;
    Outermost->getExitBlocks(ExitBlocks);
    for (const auto &BB : ExitBlocks)
      insertStreamEnd(*BB);

    return true;
  }

private:
  /// Loop chain.
  using LoopChain = SmallVector<const Loop *, 4>;
  /// Induction variable stream vector.
  using IVVec = SmallVector<const InductionVariableStream *, 4>;
  /// Memory stream vector.
  using MSVec = SmallVector<const MemoryStream *>;

  /// Address factor configuration.
  struct AddrFactorConfig {
    Value *Stride;
    const void *DepStream;
    bool IsMS;
  };

  /// Address factor configuration vector.
  using AFVec = SmallVector<AddrFactorConfig, 4>;

  /// Finds a path in loop tree by the given hints.
  LoopChain findPath(const Loop *L) {
    LoopChain Path;
    Path.push_back(L);

    while (!Path.back()->getSubLoops().empty()) {
      const auto &Subs = Path.back()->getSubLoops();
      const Loop *NextLoop;

      if (Subs.size() == 1) {
        NextLoop = Subs.front();
      } else {
        NextLoop = nullptr;
        for (const auto &Sub : Subs) {
          if (Hints.contains(Sub)) {
            if (NextLoop)
              return Path;
            NextLoop = Sub;
          }
        }
        if (!NextLoop)
          return Path;
      }

      Path.push_back(NextLoop);
    }
    return Path;
  }

  /// Checks if the given loops (the first loop is the outermost)
  /// can be streamized.
  bool canBeStreamized(const LoopChain &Loops, IVVec &IVs, MSVec &MSs) {
    auto Outermost = Loops.front();

    // Select induction variable streams.
    for (const auto &L : Loops) {
      auto IV = LSI.getIndvarStream(L);
      if (!IV)
        continue;

      // All values related to the stream must be loop invariant.
      if (!Outermost->isLoopInvariant(IV->InitVal) ||
          !SE.isLoopInvariant(IV->StepVal, Outermost) || !IV->FinalVal ||
          !Outermost->isLoopInvariant(IV->FinalVal->Value))
        continue;

      // Select by the selection strategy.
      if (SelectStrategy.getValue() < Marked || Hints.contains(L))
        IVs.push_back(IV);
    }

    // Shrink if the selected chain is too long.
    if (IVs.size() > MaxIndvarStreams.getValue()) {
      auto Strategy = SelectStrategy.getValue();
      if (Strategy >= Marked)
        Strategy = static_cast<SelectStrategyTy>(
            static_cast<unsigned>(Strategy) - static_cast<unsigned>(Marked));
      switch (Strategy) {
      default:
        llvm_unreachable("Unhandled selection strategy");
      case All:
        break;
      case AllOuter:
        IVs.resize(MaxIndvarStreams.getValue());
        break;
      case AllInner:
        std::move(IVs.end() - MaxIndvarStreams.getValue(), IVs.end(),
                  IVs.begin());
        IVs.resize(MaxIndvarStreams.getValue());
        break;
      }
    }

    // Must select at least one induction variable stream,
    // but can not more than `max-ivs`.
    if (IVs.empty() || IVs.size() > MaxIndvarStreams.getValue())
      return false;

    // Select memory streams.
    for (const auto &BB : Outermost->getBlocks()) {
      for (auto &I : *BB) {
        auto GEP = dyn_cast<GetElementPtrInst>(&I);
        if (!GEP)
          continue;
        auto MS = LSI.getMemoryStream(GEP);
        if (!MS)
          continue;

        // Check data width, and check if is accessed.
        if (MS->Width > MaxDataWidth.getValue() || (!MS->Read && !MS->Written))
          continue;

        // Check address factors.
        if (!checkAddrFactors(Outermost, IVs, MS))
          continue;

        MSs.push_back(MS);
      }
    }

    // Sort memory streams by topology order,
    // and remove invalid indirect memory streams.
    if (!NoIndirect)
      sortMemoryStreams(Outermost, MSs);

    // Must not more than `max-mss`.
    if (MSs.size() > MaxMemoryStreams.getValue()) {
      if (!TruncateMems)
        return false;
      MSs.resize(MaxMemoryStreams.getValue());
    }

    // All done.
    return true;
  }

  /// Checks if all address factors of the given memory stream are valid.
  /// This function will not check memory stream address factors.
  bool checkAddrFactors(const Loop *L, const IVVec &IVs,
                        const MemoryStream *MS) {
    bool HasBaseFactor = false;
    SmallSet<const InductionVariableStream *, 4> RefIVs;
    SmallSet<const MemoryStream *, 4> RefMSs;

    for (const auto &Factor : MS->Factors) {
      bool IsBaseFactor = false;

      // Check for dependent stream.
      const auto &Dep = Factor.DepStream;
      if (auto IV = std::get_if<InductionVariableStream *>(&Dep)) {
        // Check if this induction variable stream is in the stream vector.
        // Use linear search is enough due to number of IV streams isn't
        // expected too large.
        if (std::find(IVs.begin(), IVs.end(), *IV) != IVs.end()) {
          // Mark as an induction variable stream.
          RefIVs.insert(*IV);
        } else if (L->isLoopInvariant((*IV)->PHI)) {
          // The PHI node is loop invariant, mark as a base factor.
          HasBaseFactor = true;
          IsBaseFactor = true;
        } else {
          // Invalid induction variable stream.
          return false;
        }
      } else if (auto MS = std::get_if<MemoryStream *>(&Dep)) {
        // Check if indirect memory access is not allowed.
        if (NoIndirect)
          return false;
        // Check if is loop invariant.
        if (L->isLoopInvariant((*MS)->GEP)) {
          // Mark as a base factor.
          HasBaseFactor = true;
          IsBaseFactor = true;
        } else {
          // Mark as a memory stream, but actually it can not be determined
          // at this time.
          RefMSs.insert(*MS);
        }
      } else {
        auto V = std::get_if<Value *>(&Dep);
        assert(V && "WTF?");
        // Check if is loop invariant.
        if (L->isLoopInvariant(*V)) {
          // Mark as a base factor.
          HasBaseFactor = true;
          IsBaseFactor = true;
        } else {
          // Invalid value.
          return false;
        }
      }

      // Check for strides.
      for (const auto &Stride : Factor.Strides) {
        const auto &Value = Stride.Value;
        if (auto V = std::get_if<::Value *>(&Value)) {
          // Check if is not loop invariant.
          if (!L->isLoopInvariant(*V))
            return false;
        }
        // Div can only be applied on base factor, otherwise may lost accuracy.
        if ((Stride.Op == MemoryStream::Stride::SDiv ||
             Stride.Op == MemoryStream::Stride::UDiv) &&
            !IsBaseFactor)
          return false;
      }
    }

    // Check for number of factors.
    if (RefIVs.size() + RefMSs.size() + HasBaseFactor >
            MaxAddrFactors.getValue() ||
        (RefIVs.empty() && RefMSs.empty()))
      return false;
    return true;
  }

  /// Performs topological sort on the given memory stream vector.
  void sortMemoryStreams(const Loop *L, MSVec &MSs) {
    SmallDenseMap<const MemoryStream *, unsigned, 8> InDeg;
    for (const auto &MS : MSs)
      InDeg.insert({MS, 0});

    // Update in degrees.
    for (const auto &MS : MSs) {
      for (const auto &Factor : MS->Factors) {
        auto Ref = std::get_if<MemoryStream *>(&Factor.DepStream);
        if (!Ref || L->isLoopInvariant((*Ref)->GEP))
          continue;

        if (auto It = InDeg.find(*Ref); It != InDeg.end()) {
          ++It->second;
        } else {
          // Memory stream references an invalid memory stream.
          InDeg.erase(MS);
          break;
        }
      }
    }

    // Enqueue streams that have no dependencies.
    std::queue<const MemoryStream *> Q;
    for (const auto &[MS, D] : InDeg) {
      if (!D)
        Q.push(MS);
    }

    // Sort.
    MSs.clear();
    while (!Q.empty()) {
      auto MS = Q.front();
      Q.pop();

      bool IsValid = true;
      for (const auto &Factor : MS->Factors) {
        auto Ref = std::get_if<MemoryStream *>(&Factor.DepStream);
        if (!Ref || L->isLoopInvariant((*Ref)->GEP))
          continue;

        if (auto It = InDeg.find(*Ref); It != InDeg.end()) {
          if (!--It->second)
            Q.push(*Ref);
        } else {
          // Memory stream references an invalid memory stream.
          InDeg.erase(MS);
          IsValid = false;
          break;
        }
      }

      if (IsValid)
        MSs.push_back(MS);
    }

    // Check sanity.
    for (const auto &[_, D] : InDeg) {
      assert(!D && "Cycle detected in memory stream dependency graph!");
    }
  }

  /// Inserts stream configuration intrinsics (include `ready`)
  /// to the given block.
  void insertStreamConfigs(const Loop *L, BasicBlock &BB, const IVVec &IVs,
                           const MSVec &MSs) {
    IRBuilder<> Builder(BB.getTerminator());
    DenseMap<const void *, unsigned> IDs;
    unsigned LastID = 0, ConfigNum = 0;

    // Insert induction variable stream configs.
    SCEVExpander Exp(SE, SE.getDataLayout(), "smx.streamize");
    Exp.setInsertPoint(BB.getTerminator());
    for (const auto &IV : IVs) {
      // FIXME: This is tricky and unreliable.
      bool IsSigned = CmpInst::isSigned(IV->FinalVal->Cond);

      auto Init = castToSizeTy(Builder, IV->InitVal, IsSigned);
      auto Step = Exp.expandCodeFor(IV->StepVal);
      Step = castToSizeTy(Builder, Step, IsSigned);
      auto Final = castToSizeTy(Builder, IV->FinalVal->Value, IsSigned);
      auto Cond = getSizeTyInt(predToStopCond(IV->FinalVal->Cond));

      Builder.CreateCall(Intrinsic::getDeclaration(F.getParent(),
                                                   Intrinsic::riscv_smx_cfg_iv,
                                                   {SizeTy}),
                         {Init, Step, Final, Cond});
      ++ConfigNum;

      // Update ID.
      IDs.insert({IV, LastID++});
    }

    // Insert memory stream configs and address factor configs.
    LastID = 0;
    for (const auto &MS : MSs) {
      Value *Base;
      AFVec AFs;
      getMemoryStreamInfoForConfig(L, Builder, MS, Base, AFs);

      // Insert memory stream config.
      auto BasePtr = Builder.CreateIntToPtr(Base, Builder.getPtrTy());
      assert(!AFs.empty() && "No address factor in memory stream!");
      const auto &FirstAF = AFs.front();
      auto Dep = getSizeTyInt(IDs.find(FirstAF.DepStream)->second);
      auto Kind = getSizeTyInt(FirstAF.IsMS);
      assert((MS->Read || MS->Written) && "Memory stream does not access memory!");
      auto Prefetch = getSizeTyInt(1);
      auto Width = getSizeTyInt(Log2_32_Ceil(MS->Width));
      Builder.CreateCall(Intrinsic::getDeclaration(F.getParent(),
                                                   Intrinsic::riscv_smx_cfg_ms,
                                                   {SizeTy}),
                         {BasePtr, FirstAF.Stride, Dep, Kind, Prefetch, Width});
      ++ConfigNum;

      // Insert the rest address factor configs.
      for (unsigned Idx = 1; Idx < AFs.size(); Idx += 2) {
        const auto &AF = AFs[Idx];
        auto Dep1 = getSizeTyInt(IDs.find(AF.DepStream)->second);
        auto Kind1 = getSizeTyInt(AF.IsMS);

        Value *Stride2, *Dep2, *Kind2;
        if (Idx + 1 < AFs.size()) {
          const auto &AF = AFs[Idx + 1];
          Stride2 = AF.Stride;
          Dep2 = getSizeTyInt(IDs.find(AF.DepStream)->second);
          Kind2 = getSizeTyInt(AF.IsMS);
        } else {
          Stride2 = getSizeTyInt(0);
          Dep2 = getSizeTyInt(0);
          Kind2 = getSizeTyInt(0);
        }

        Builder.CreateCall(
            Intrinsic::getDeclaration(F.getParent(),
                                      Intrinsic::riscv_smx_cfg_addr, {SizeTy}),
            {AF.Stride, Dep1, Kind1, Stride2, Dep2, Kind2});
        ++ConfigNum;
      }

      // Update ID.
      IDs.insert({MS, LastID++});
    }

    // Insert ready.
    Builder.CreateCall(Intrinsic::getDeclaration(
                           F.getParent(), Intrinsic::riscv_smx_ready, {SizeTy}),
                       {getSizeTyInt(ConfigNum)});
  }

  /// Gets necessary information for inserting memory stream configs.
  /// Inserts new instructions to the basic block on necessary.
  void getMemoryStreamInfoForConfig(const Loop *L, IRBuilder<> &Builder,
                                    const MemoryStream *MS, Value *&Base,
                                    AFVec &AFs) {
    Base = getSizeTyInt(0);
    DenseMap<const void *, unsigned> AFEntries;

    // FIXME: NSW/NUW/... information may required.
    for (const auto &Factor : MS->Factors) {
      Value *Init = nullptr;
      const void *DepStream = nullptr;
      bool IsMS = false;

      // Check dependent stream.
      const auto &Dep = Factor.DepStream;
      if (auto IV = std::get_if<InductionVariableStream *>(&Dep)) {
        if (L->isLoopInvariant((*IV)->PHI)) {
          Init = (*IV)->PHI;
        } else {
          DepStream = *IV;
        }
      } else if (auto MS = std::get_if<MemoryStream *>(&Dep)) {
        if (L->isLoopInvariant((*MS)->GEP)) {
          Init = (*MS)->GEP;
        } else {
          DepStream = *MS;
          IsMS = true;
        }
      } else {
        auto V = std::get_if<Value *>(&Dep);
        assert(V && "WTF?");
        Init = *V;
      }

      if (Init) {
        // Update base address.
        reduceAndUpdate(
            Builder, Base,
            reduceStrides(Builder, castToSizeTy(Builder, Init, false),
                          ArrayRef<MemoryStream::Stride>(Factor.Strides)),
            Factor.IsNeg);
      } else {
        // Generate stride.
        auto Stride =
            reduceStrides(Builder, getSizeTyInt(1),
                          ArrayRef<MemoryStream::Stride>(Factor.Strides));

        // Update address factor vector.
        if (auto It = AFEntries.find(DepStream); It != AFEntries.end()) {
          auto &AF = AFs[It->second];
          reduceAndUpdate(Builder, AF.Stride, Stride, Factor.IsNeg);
        } else {
          if (Factor.IsNeg)
            Stride = Builder.CreateNeg(Stride);
          AFEntries.insert({DepStream, AFs.size()});
          AFs.push_back({Stride, DepStream, IsMS});
        }
      }
    }
  }

  /// Generates binary operation of the two values,
  /// and then update the first value with the result.
  void reduceAndUpdate(IRBuilder<> &Builder, Value *&First, Value *Second,
                       Instruction::BinaryOps Opc) {
    Second = castToSizeTy(Builder, Second, false);
    First = Builder.CreateBinOp(Opc, First, Second);
  }

  /// Generates add/sub of the two values,
  /// and then update the first value with the result.
  void reduceAndUpdate(IRBuilder<> &Builder, Value *&First, Value *Second,
                       bool IsNeg) {
    reduceAndUpdate(Builder, First, Second,
                    IsNeg ? Instruction::Sub : Instruction::Add);
  }

  /// Reduces the given strides to a LLVM value.
  Value *reduceStrides(IRBuilder<> &Builder, Value *Init,
                       ArrayRef<MemoryStream::Stride> Strides) {
    for (const auto &Stride : Strides) {
      // Get the current stride.
      Value *RHS;
      if (auto V = std::get_if<Value *>(&Stride.Value)) {
        RHS = *V;
      } else {
        auto C = std::get_if<unsigned>(&Stride.Value);
        assert(C && "WTF?");
        RHS = getSizeTyInt(*C);
      }

      // Get the binary opcode.
      Instruction::BinaryOps Opc;
      switch (Stride.Op) {
      default:
        llvm_unreachable("Unhandled stride op");
      case MemoryStream::Stride::Mul:
        Opc = Instruction::Mul;
        break;
      case MemoryStream::Stride::Shl:
        Opc = Instruction::Shl;
        break;
      case MemoryStream::Stride::SDiv:
        Opc = Instruction::SDiv;
        break;
      case MemoryStream::Stride::UDiv:
        Opc = Instruction::UDiv;
        break;
      }

      // Reduce.
      reduceAndUpdate(Builder, Init, RHS, Opc);
    }
    return Init;
  }

  /// Replaces all related induction variable update with stream steps.
  void replaceIndvarUpdates(const IVVec &IVs) {
    for (unsigned Idx = 0; Idx < IVs.size(); ++Idx) {
      const auto &IV = IVs[Idx];

      // Get the latch block.
      auto Latch = IV->Loop->getLoopLatch();
      assert(Latch && "Invalid loop latch!");
      auto Term = Latch->getTerminator();

      // Insert step intrinsic.
      IRBuilder<> Builder(Term);
      auto Step = Builder.CreateCall(
          Intrinsic::getDeclaration(F.getParent(), Intrinsic::riscv_smx_step,
                                    {SizeTy}),
          {getSizeTyInt(Idx)});
      // FIXME: Remove this unnecessary truncate
      //        if more overload of `smx_step` is available.
      auto TruncStep = Builder.CreateTrunc(Step, IV->PHI->getType());

      // Update the step value in the PHI node.
      auto OldStep = IV->PHI->getIncomingValueForBlock(Latch);
      OldStep->replaceAllUsesWith(TruncStep);

      // Check if the terminator is a conditional branch.
      auto Br = dyn_cast<BranchInst>(Term);
      if (!Br || !Br->isConditional())
        continue;

      // Insert stop value intrinsic.
      auto Stop = Builder.CreateCall(
          Intrinsic::getDeclaration(F.getParent(),
                                    Intrinsic::riscv_smx_stop_val, {SizeTy}),
          {getSizeTyInt(Idx)});

      // Update the terminator of the latch block.
      auto OldCmp = Br->getCondition();
      if (Br->getSuccessor(0) == IV->Loop->getHeader()) {
        Br->setCondition(Builder.CreateICmpNE(Step, Stop));
      } else {
        Br->setCondition(Builder.CreateICmpEQ(Step, Stop));
      }
      if (auto I = dyn_cast<Instruction>(OldCmp))
        I->eraseFromParent();
    }
  }

  /// Replaces all related memory accesses with stream accesses.
  void replaceMemoryAccesses(const MSVec &MSs) {
    for (unsigned Idx = 0; Idx < MSs.size(); ++Idx) {
      const auto &MS = MSs[Idx];
      auto Ty = MS->GEP->getResultElementType();

      for (const auto &Use : MS->GEP->uses()) {
        if (auto Load = dyn_cast<LoadInst>(Use.getUser())) {
          // Replace with a load intrinsic.
          IRBuilder<> Builder(Load);
          Load->replaceAllUsesWith(Builder.CreateCall(
              Intrinsic::getDeclaration(
                  F.getParent(), Intrinsic::riscv_smx_load, {Ty, SizeTy}),
              {getSizeTyInt(Idx), getSizeTyInt(0)}));
        } else if (auto Store = dyn_cast<StoreInst>(Use.getUser())) {
          // Replace with a store intrinsic.
          IRBuilder<> Builder(Store);
          Builder.CreateCall(
              Intrinsic::getDeclaration(
                  F.getParent(), Intrinsic::riscv_smx_store, {SizeTy, Ty}),
              {getSizeTyInt(Idx), getSizeTyInt(0), Store->getValueOperand()});
          Store->eraseFromParent();
        }
      }
    }
  }

  /// Inserts stream end intrinsics to the given block.
  void insertStreamEnd(BasicBlock &BB) {
    Instruction *IP;
    if (auto LandingPad = BB.getLandingPadInst()) {
      IP = LandingPad->getNextNode();
    } else {
      IP = BB.getFirstNonPHI();
    }
    IRBuilder<>(IP).CreateCall(
        Intrinsic::getDeclaration(F.getParent(), Intrinsic::riscv_smx_end));
  }

  /// Casts the given integer/pointer to a value of type `size_t`.
  Value *castToSizeTy(IRBuilder<> &Builder, Value *V, bool IsSigned) {
    assert(V->getType()->isIntOrPtrTy() && "Invalid integer or pointer!");
    if (V->getType()->isIntegerTy())
      return Builder.CreateIntCast(V, SizeTy, IsSigned);
    else
      return Builder.CreatePtrToInt(V, SizeTy);
  }

  /// Gets a integer constant of type `size_t`.
  Value *getSizeTyInt(uint64_t C) { return ConstantInt::get(SizeTy, C); }

  Function &F;
  const SmallSet<const Loop *, 4> &Hints;
  ScalarEvolution &SE;
  LoopStreamInfo LSI;
  Type *SizeTy;
};

} // namespace

PreservedAnalyses LoopStreamization::run(Function &F,
                                         FunctionAnalysisManager &FAM) const {
  auto &LI = FAM.getResult<LoopAnalysis>(F);

  // Find all loops that are marked as `streamize`.
  SmallSet<const Loop *, 4> HintedLoops;
  for (const auto &Loop : LI) {
    if (shouldBeStreamized(Loop->getLoopID()))
      HintedLoops.insert(Loop);
  }

  // Quit if nothing found.
  if (HintedLoops.empty())
    return PreservedAnalyses::all();

  // Pick outermost loops.
  SmallVector<const Loop *> CandidateLoops;
  unsigned MinDepth = -1;
  for (const auto &Loop : HintedLoops) {
    unsigned Depth = Loop->getLoopDepth();
    if (Depth < MinDepth) {
      MinDepth = Depth;
      CandidateLoops.clear();
    }
    if (Depth == MinDepth)
      CandidateLoops.push_back(Loop);
  }

  // Try to streamize all candidate loops.
  bool Changed = false;
  auto &SE = FAM.getResult<ScalarEvolutionAnalysis>(F);
  auto &SIs = FAM.getResult<StreamMemoryAnalysis>(F);
  Streamizer S(F, HintedLoops, SE, SIs);
  for (const auto &Loop : CandidateLoops)
    Changed |= S.tryOn(Loop);
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

void registerLoopStreamization(llvm::PassBuilder &PB) {
  PB.registerPipelineParsingCallback(
      [](StringRef Name, FunctionPassManager &FPM,
         ArrayRef<PassBuilder::PipelineElement>) {
        if (Name == "loop-streamization") {
          FPM.addPass(LoopStreamization());
          return true;
        }
        return false;
      });
}
