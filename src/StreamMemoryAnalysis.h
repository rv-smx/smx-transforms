#ifndef SMX_TRANSFORMS_STREAM_MEMORY_ANALYSIS_H
#define SMX_TRANSFORMS_STREAM_MEMORY_ANALYSIS_H

#include <cstdint>
#include <memory>

#include "llvm/ADT/Optional.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/raw_ostream.h"

/// Induction variable stream, corresponding to a PHI node.
struct InductionVariableStream {
  struct IVValue {
    /// True if the value is constant.
    bool IsConstant;
    /// The constant value (lowest 64-bit).
    std::uint64_t Value;

    void print(llvm::raw_ostream &OS) const;
  };

  /// Name.
  llvm::StringRef Name;
  /// Depth of the loop that this induction variable is located.
  unsigned LoopDepth;
  /// True if is a canonical induction variable.
  bool IsCanonical;
  /// Initial value.
  llvm::Optional<IVValue> InitVal;
  /// Final value.
  llvm::Optional<IVValue> FinalVal;
  /// True if this induction variable is sure to be increasing.
  bool IsIncreasing;
  /// Opcode of the step instruction.
  llvm::Optional<unsigned> StepInstOpc;

  void print(llvm::raw_ostream &OS) const;
};

/// Memory stream, corresponding to a GEP.
struct MemoryStream {
  struct AddressFactor {
    /// Dependent stream.
    void *DepStream;
    /// Kind of the dependent stream.
    enum {
      /// Induction variable stream.
      InductionVariable,
      /// Memory stream.
      Memory,
      /// Not a stream, just a LLVM value.
      NotAStream,
    } DepStreamKind;
    /// Stride.
    unsigned Stride;
    /// Is loop invariant.
    bool IsInvariant;

    void print(llvm::raw_ostream &OS) const;
  };

  /// Name.
  llvm::StringRef Name;
  /// Result type.
  llvm::Type *ResultType;
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
  // does not access memory stream.
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
