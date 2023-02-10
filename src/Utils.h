#ifndef SMX_TRANSFORMS_UTILS_H
#define SMX_TRANSFORMS_UTILS_H

#include <string>

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/Support/raw_ostream.h"

/// Prints string to the given output stream.
void printString(llvm::raw_ostream &OS, llvm::StringRef Str);

/// Converts object with `print` method to a string.
template <typename T> inline std::string convPrintableToStr(const T &Obj) {
  std::string Str;
  llvm::raw_string_ostream SS(Str);
  Obj.print(SS);
  SS.flush();
  return Str;
}

/// Prints object with `print` method to the given output stream.
template <typename T>
inline void printPrintable(llvm::raw_ostream &OS, const T &Obj) {
  printString(OS, convPrintableToStr(Obj));
}

#endif // SMX_TRANSFORMS_UTILS_H
