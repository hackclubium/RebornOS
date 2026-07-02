#!/usr/bin/env bash
# Builds a real x86_64-elf-gcc + binutils cross-compiler from source into
# $HOME/opt/cross. No sudo needed: everything installs under $HOME.
# Idempotent -- re-running after a successful build is a no-op.
set -euo pipefail

TARGET=x86_64-elf
PREFIX="$HOME/opt/cross"
BINUTILS_VERSION=2.42
GCC_VERSION=13.2.0
JOBS="$(nproc)"
WORK="$HOME/opt/cross-build"

if [ -x "$PREFIX/bin/$TARGET-gcc" ]; then
    echo "Cross compiler already present at $PREFIX/bin/$TARGET-gcc"
    "$PREFIX/bin/$TARGET-gcc" --version | head -1
    exit 0
fi

mkdir -p "$WORK" "$PREFIX"
export PATH="$PREFIX/bin:$PATH"
cd "$WORK"

echo "==> binutils $BINUTILS_VERSION"
if [ ! -f "binutils-$BINUTILS_VERSION.tar.xz" ]; then
    wget -q "https://ftp.gnu.org/gnu/binutils/binutils-$BINUTILS_VERSION.tar.xz"
fi
if [ ! -d "binutils-$BINUTILS_VERSION" ]; then
    tar xf "binutils-$BINUTILS_VERSION.tar.xz"
fi
mkdir -p build-binutils
(
    cd build-binutils
    "../binutils-$BINUTILS_VERSION/configure" --target=$TARGET --prefix="$PREFIX" \
        --with-sysroot --disable-nls --disable-werror
    make -j"$JOBS"
    make install
)

echo "==> gcc $GCC_VERSION (stage 1: C only, no libc, just enough for a freestanding kernel/bootloader)"
if [ ! -f "gcc-$GCC_VERSION.tar.xz" ]; then
    wget -q "https://ftp.gnu.org/gnu/gcc/gcc-$GCC_VERSION/gcc-$GCC_VERSION.tar.xz"
fi
if [ ! -d "gcc-$GCC_VERSION" ]; then
    tar xf "gcc-$GCC_VERSION.tar.xz"
fi
mkdir -p build-gcc
(
    cd build-gcc
    "../gcc-$GCC_VERSION/configure" --target=$TARGET --prefix="$PREFIX" \
        --disable-nls --enable-languages=c --without-headers
    make -j"$JOBS" all-gcc
    make -j"$JOBS" all-target-libgcc
    make install-gcc
    make install-target-libgcc
)

echo "==> done: $PREFIX/bin/$TARGET-gcc"
"$PREFIX/bin/$TARGET-gcc" --version | head -1
