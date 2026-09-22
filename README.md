# xeno
use freertos support pthread functions and gcc c++ thread for baremetal, with an arm simulator for test

you must use toolchain with gcc-16 and cmake 4.3 or later

I can give toolchains in this repo.

# build and sim

```
cmake -S . -B build -G Ninja --fresh
cmake --build build -j -t sim
```