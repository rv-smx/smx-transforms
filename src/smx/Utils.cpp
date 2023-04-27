#include "Utils.h"

#include "llvm/ADT/StringExtras.h"

using namespace llvm;

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
