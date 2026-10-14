#!/usr/bin/env bash
set -e
cmake -S . -B build -G Ninja --fresh
cmake --build build -j -t sim