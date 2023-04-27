add_library(LoopProfiler MODULE
  LoopProfiler.cpp
)

set_target_properties(
  LoopProfiler PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}
)
