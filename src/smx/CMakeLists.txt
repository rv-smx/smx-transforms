add_library(SMXTransforms MODULE
  LoopStreamization.cpp
  LoopTreePrinter.cpp
  PassRegistry.cpp
  StreamMemoryAnalysis.cpp
  Utils.cpp
)

set_target_properties(
  SMXTransforms PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}
)
