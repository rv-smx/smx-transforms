# SMX Transforms

This repository contains LLVM transforms developed for the RISC-V Stream-based memory access extension (SMX).

## Building This Project

Run the following command lines:

```
cd smx-transforms
mkdir build
cd build
cmake -G Ninja -DLLVM_DIR=/path/to/llvm/installation -DCMAKE_BUILD_TYPE=Release ..
ninja
```

## Running with `opt`

You should [build this project](#building-this-project) first.

Run the SMX analysis for the LLVM IR file `input.ll`:

```
opt -load-pass-plugin=/path/to/smx-transforms/src/libSMXTransforms.so \
    -passes="instnamer,loop-simplify,print<stream-memory>" \
    -disable-output -march=rv46gc_xsmx \
    input.ll
```

Run the SMX transform:

> WIP.

## License

[Apache License 2.0](LICENSE).
