#!/bin/sh

echo "error: this project configures llama.cpp with hermetic CMake and Ninja; Make, M4, and pkg-config must remain unused" >&2
exit 1
