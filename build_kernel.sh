#!/bin/bash

# Define paths and toolchain
PREFIX="$(pwd)"

# Check if custom LLVM toolchain exists, otherwise use default
if [ -d "/home/zears/tc-build/build/llvm/final/bin" ]; then
    CLANG_DIR="/home/zears/tc-build/build/llvm/final"
    echo "Using custom LLVM toolchain: $CLANG_DIR"
else
    CLANG_DIR="${PREFIX}/toolchain/clang/host/linux-x86/clang-r383902"
    echo "Using default toolchain: $CLANG_DIR"
fi

# Set up PATH for clang/llvm tools
export PATH="$CLANG_DIR/bin:$PATH"
export ARCH=arm64

# Build configuration
export KCFLAGS=-w
export CONFIG_SECTION_MISMATCH_WARN_ONLY=y

echo "Using PATH: $PATH"

# Configure kernel
make -C $(pwd) O=$(pwd)/out ARCH=arm64 a22_defconfig

# Build kernel with clang/llvm + ccache
make -j16 ARCH=arm64 SUBARCH=arm64 O=out \
    CC="ccache clang" \
    AR="llvm-ar" \
    NM="llvm-nm" \
    LD="ld.lld" \
    OBJCOPY="llvm-objcopy" \
    OBJDUMP="llvm-objdump" \
    STRIP="llvm-strip" \
    CLANG_TRIPLE="aarch64-linux-gnu-" \
    CROSS_COMPILE="aarch64-linux-gnu-" \
    CROSS_COMPILE_ARM32="arm-linux-gnueabi-" \
    CROSS_COMPILE_COMPAT="arm-linux-gnueabi-" \
    LLVM=1 \
    LLVM_IAS=1 \
    INSTALL_MOD_STRIP=1 \
    KCFLAGS=-w \
    CONFIG_SECTION_MISMATCH_WARN_ONLY=y \
    KBUILD_BUILD_USER="$(git rev-parse --short HEAD | cut -c1-7)" \
    KBUILD_BUILD_HOST="$(git symbolic-ref --short HEAD)"

# Copy the built kernel image
cp out/arch/arm64/boot/Image $(pwd)/arch/arm64/boot/Image

# Show ccache statistics
ccache -s