#ifndef SMX_TRANSFORMS_STREAM_MEMORY_ANALYSIS_H
#define SMX_TRANSFORMS_STREAM_MEMORY_ANALYSIS_H

#include <cstdint>
#include <memory>
#include <variant>

#include "llvm/ADT/Optional.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/raw_ostream.h"

/// Induction variable stream, corresponding to a PHI node.
struct InductionVariableStream {
  struct FinalValue {
    /// The corresponding LLVM value.
    llvm::Value *Value;
    /// True if the value is loop invariant.
    bool IsInvariant;
    /// Condition for loop exit.
    llvm::CmpInst::Predicate Cond;

    void print(llvm::raw_ostream &OS) const;
  };

  /// PHI corresponding to this memory stream.
  llvm::PHINode *PHI;
  /// The parent induction variable stream,
  /// `nullptr` if this is the outermost induction variable.
  InductionVariableStream *Parent;
  /// The loop that this induction variable is located.
  llvm::Loop *Loop;
  /// True if is a canonical induction variable.
  bool IsCanonical;
  /// Initial value.
  llvm::Value *InitVal;
  /// Step value.
  const llvm::SCEV *StepVal;
  /// Final value.
  llvm::Optional<FinalValue> FinalVal;

  void print(llvm::raw_ostream &OS) const;
};

/// Memory stream, corresponding to a GEP.
struct MemoryStream {
  struct Stride {
    /// Stride value.
    std::variant<unsigned, llvm::Value *> Value;
    /// Operator of the stride.
    enum {
      /// (* stride).
      Mul,
      /// (<< stride), or, (* (1 << stride)).
      Shl,
      /// (/ stride), signed.
      SDiv,
      /// (/ stride), unsigned.
      UDiv,
    } Op;
    /// Is loop invariant.
    bool IsInvariant;

    void print(llvm::raw_ostream &OS) const;
  };

  struct AddressFactor {
    /// Dependent stream.
    std::variant<InductionVariableStream *, MemoryStream *, llvm::Value *>
        DepStream;
    /// Strides.
    llvm::SmallVector<Stride, 1> Strides;
    /// Is loop invariant.
    bool IsInvariant;
    /// `true` if (- factor), otherwise (+ factor).
    bool IsNeg;

    void print(llvm::raw_ostream &OS) const;
  };

  /// GEP corresponding to this memory stream.
  llvm::GetElementPtrInst *GEP;
  /// Address factors.
  llvm::SmallVector<AddressFactor, 4> Factors;
  /// Has been read.
  bool Read;
  /// Has been written.
  bool Written;
  /// Width of the memory access in bytes.
  unsigned Width;

  void print(llvm::raw_ostream &OS) const;
};

/// Memory operation, corresponding to a load/store instruction.
struct MemoryOperation {
  /// Opcode of the memory instruction.
  unsigned MemOpc;
  /// Memory stream, `nullptr` if this operation
  /// does not access memory stream.
  MemoryStream *MemStream;

  void print(llvm::raw_ostream &OS) const;
};

struct StreamInfo {
  /// The parent loop.
  llvm::Loop *Loop;
  /// Induction variable streams.
  llvm::SmallVector<std::unique_ptr<InductionVariableStream>, 4> IVs;
  /// Memory streams.
  llvm::SmallVector<std::unique_ptr<MemoryStream>, 4> MemStreams;
  /// Memory operations.
  llvm::SmallVector<std::unique_ptr<MemoryOperation>, 4> MemOps;

  void print(llvm::raw_ostream &OS) const;
};

class StreamMemoryAnalysis
    : public llvm::AnalysisInfoMixin<StreamMemoryAnalysis> {
public:
  using Result = llvm::SmallVector<StreamInfo, 0>;

  Result run(llvm::Function &F, llvm::FunctionAnalysisManager &FAM) const;

private:
  static llvm::AnalysisKey Key;
  friend struct llvm::AnalysisInfoMixin<StreamMemoryAnalysis>;
};

class StreamMemoryAnalysisPrinter
    : public llvm::PassInfoMixin<StreamMemoryAnalysisPrinter> {
public:
  explicit StreamMemoryAnalysisPrinter(llvm::raw_ostream &OS) : OS(OS) {}

  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &FAM) const;

private:
  llvm::raw_ostream &OS;
};

void registerStreamMemoryAnalysis(llvm::PassBuilder &PB);

#endif // SMX_TRANSFORMS_STREAM_MEMORY_ANALYSIS_H
