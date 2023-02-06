#ifndef SMX_TRANSFORMS_UTILS_H
#define SMX_TRANSFORMS_UTILS_H

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/Support/raw_ostream.h"

/// Prints string to the given output stream.
void printString(llvm::raw_ostream &OS, llvm::StringRef Str);

/// Prints debug info location to the given output stream.
void printDebugLoc(llvm::raw_ostream &OS, llvm::DebugLoc Loc);

#endif // SMX_TRANSFORMS_UTILS_H
