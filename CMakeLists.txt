cmake_minimum_required(VERSION 3.13.4)
project(SMXTransforms)

set(CMAKE_CXX_STANDARD 17)

# Find LLVM package.
find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

# Add definitions, includes, libraries about LLVM.
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
add_definitions(${LLVM_DEFINITIONS_LIST})
include_directories(${LLVM_INCLUDE_DIRS} src)
link_directories(${LLVM_LIBRARY_DIRS})

# Set C++ compiler flags.
SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Werror")
if(NOT LLVM_ENABLE_RTTI)
  # Disable RTTI if LLVM does not have it enabled to prevent load time errors.
  SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti")
endif()

add_subdirectory(src)
