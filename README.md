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

> WIP.

## License

[Apache License 2.0](LICENSE).
