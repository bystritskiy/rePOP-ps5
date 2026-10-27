FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates git curl wget unzip zip xz-utils rsync build-essential cmake ninja-build pkg-config python3 python3-pip python3-venv python3-mako python3-yaml python3-ply clang-18 llvm-18 llvm-18-dev libclang-18-dev libllvmspirvlib-18-dev libclc-18-dev libzstd-dev zlib1g-dev libexpat1-dev bison flex meson spirv-tools libclc-18 && rm -rf /var/lib/apt/lists/*
ENV LLVM_CONFIG=/usr/bin/llvm-config-18
ENV CC=clang-18 CXX=clang++-18
RUN apt-get update && apt-get install -y --no-install-recommends lld-18 glslang-tools && rm -rf /var/lib/apt/lists/*
