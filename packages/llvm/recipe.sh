# llvm: clang and lld for sqterm's prefix (step 3, first spike), LLVM 23.
#
# One binary, bin/llvm (LLVM's driver build), with clang, clang++, ld.lld and
# the rest as symbolic links to it: one copy of LLVM's code, not one per
# tool. Built for API 28 with the NDK's CMake toolchain, the C++ library
# linked in. clang defaults to libc++, compiler-rt and lld, and to sqterm's
# target (aarch64-linux-android28). The C++ library and runtimes for what
# clang builds, the sysroot, and the other tools (clangd, clang-tidy,
# clang-doc, lldb, compiler-rt) come in later packages. No plugins: a
# plugin is a shared library that leaves clang's symbols for load time, which
# the NDK's link refuses (and one binary has none to give).
#
# Large: not in the APK (bundled=no). tools/fetch_packages.sh --get llvm
# downloads it; in sqterm, pkg install it --consume.
#
# Built on CI: hours on a phone. It needs CMake and Ninja, and a C++
# compiler for this machine (CXX_FOR_BUILD) for LLVM's tablegen, which runs
# while LLVM builds. LLVM publishes no sha256 (a signature only): the first
# download is recorded in packages/sources.lock.

version=23.1.5
license="Apache-2.0 WITH LLVM-exception"
file=llvm-project-$version.src.tar.xz
url=https://github.com/llvm/llvm-project/releases/download/llvmorg-$version/$file
mirrors=
sha256=
top=llvm-project-$version.src
depends=
bundled=no
keep="bin lib/clang share/licenses/llvm"

# What is built and installed: the driver (bin/llvm) and the tools linked to
# it, clang's builtin headers. Not the rest of clang's tools, each a full
# static binary of its own (clang-repl alone was 91 MB): the first build
# installed everything and came to 130 MB packed.
components="llvm-driver;clang;clang-resource-headers;lld;clang-format;clang-scan-deps"
components="$components;llvm-ar;llvm-nm;llvm-objcopy;llvm-objdump;llvm-readobj"
components="$components;llvm-symbolizer;llvm-size;llvm-strings;llvm-cxxfilt"

recipe_build() {
  [ -n "${SQ_NDK:-}" ] || { echo "llvm is built for Android only (the NDK's CMake toolchain)" >&2; return 1; }
  local -a launch=()
  if command -v ccache >/dev/null; then
    launch=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
  fi
  local common=(-G Ninja -DLLVM_TARGETS_TO_BUILD=AArch64
                -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF
                -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_DOCS=OFF
                -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF
                -DLLVM_ENABLE_LIBEDIT=OFF -DLLVM_ENABLE_LIBPFM=OFF "${launch[@]}")
  # 1. tablegen for this machine (it runs while LLVM builds), without the
  #    Android flags build.sh exports.
  env -u CFLAGS -u CXXFLAGS -u LDFLAGS -u CPPFLAGS \
    cmake -S llvm -B ../native "${common[@]}" -DCMAKE_BUILD_TYPE=Release \
      -DLLVM_ENABLE_PROJECTS=clang \
      -DCMAKE_C_COMPILER="${CC_FOR_BUILD:-cc}" -DCMAKE_CXX_COMPILER="${CXX_FOR_BUILD:-c++}"
  ninja -C ../native -j"$JOBS" llvm-tblgen clang-tblgen
  # 2. clang and lld for Android, in one binary.
  env -u CFLAGS -u CXXFLAGS -u LDFLAGS -u CPPFLAGS \
    cmake -S llvm -B ../android "${common[@]}" -DCMAKE_BUILD_TYPE=MinSizeRel \
      -DCMAKE_TOOLCHAIN_FILE="$SQ_NDK/build/cmake/android.toolchain.cmake" \
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM="android-$SQ_API" -DANDROID_STL=c++_static \
      -DCMAKE_INSTALL_PREFIX="$SQ_PREFIX" \
      -DLLVM_ENABLE_PROJECTS="clang;lld" \
      -DLLVM_HOST_TRIPLE="aarch64-unknown-linux-android$SQ_API" \
      -DLLVM_DEFAULT_TARGET_TRIPLE="aarch64-unknown-linux-android$SQ_API" \
      -DLLVM_NATIVE_TOOL_DIR="$PWD/../native/bin" \
      -DCROSS_TOOLCHAIN_FLAGS_NATIVE="-DCMAKE_C_COMPILER=${CC_FOR_BUILD:-cc};-DCMAKE_CXX_COMPILER=${CXX_FOR_BUILD:-c++}" \
      -DLLVM_TOOL_LLVM_DRIVER_BUILD=ON -DLLVM_INSTALL_TOOLCHAIN_ONLY=ON \
      -DLLVM_ENABLE_PLUGINS=OFF -DCLANG_PLUGIN_SUPPORT=OFF -DCLANG_BUILD_EXAMPLES=OFF \
      -DCLANG_DEFAULT_CXX_STDLIB=libc++ -DCLANG_DEFAULT_RTLIB=compiler-rt \
      -DCLANG_DEFAULT_UNWINDLIB=libunwind -DCLANG_DEFAULT_LINKER=lld \
      -DCMAKE_EXE_LINKER_FLAGS="-Wl,-z,max-page-size=16384" \
      -DLLVM_DISTRIBUTION_COMPONENTS="$components"
  ninja -C ../android -j"$JOBS" distribution
  DESTDIR="$dest" ninja -C ../android install-distribution
}

recipe_after() {
  mkdir -p share/licenses/llvm
  cp "$src/llvm/LICENSE.TXT" share/licenses/llvm/LICENSE.TXT
}
