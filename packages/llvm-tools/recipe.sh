# llvm-tools: clangd, clang-tidy and clang-doc for sqterm's prefix, LLVM 23
# (step 3). The editor and checking tools beside the compiler in llvm: a
# package of their own, so the compiler stays small.
#
# Each is a static binary (LLVM's driver build takes none of them), built
# with the NDK's CMake toolchain as llvm is, the C++ library linked in.
# lldb is a package of its own: its core is a shared library, liblldb.so,
# and a program and its shared library need one shared C++ library between
# them, not a static copy each (the runtimes' libc++_shared.so).
#
# Large: not in the APK (bundled=no). tools/fetch_packages.sh --get
# llvm-tools; in sqterm, pkg install it --consume.

version=23.1.5
license="Apache-2.0 WITH LLVM-exception"
file=llvm-project-$version.src.tar.xz
url=https://github.com/llvm/llvm-project/releases/download/llvmorg-$version/$file
mirrors=
sha256=
top=llvm-project-$version.src
depends=
bundled=no
keep="bin share/clang-doc share/licenses/llvm-tools"

components="clangd;clang-tidy;clang-doc"

recipe_build() {
  [ -n "${SQ_NDK:-}" ] || { echo "llvm-tools is built for Android only (the NDK's CMake toolchain)" >&2; return 1; }
  local -a launch=()
  if command -v ccache >/dev/null; then
    launch=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
  fi
  local common=(-G Ninja -DLLVM_TARGETS_TO_BUILD=AArch64
                -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF
                -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_DOCS=OFF
                -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF
                -DLLVM_ENABLE_LIBEDIT=OFF -DLLVM_ENABLE_LIBPFM=OFF "${launch[@]}")
  # 1. tablegen for this machine (it runs while LLVM builds).
  env -u CFLAGS -u CXXFLAGS -u LDFLAGS -u CPPFLAGS \
    cmake -S llvm -B ../native "${common[@]}" -DCMAKE_BUILD_TYPE=Release \
      -DLLVM_ENABLE_PROJECTS="clang;clang-tools-extra" \
      -DCMAKE_C_COMPILER="${CC_FOR_BUILD:-cc}" -DCMAKE_CXX_COMPILER="${CXX_FOR_BUILD:-c++}"
  ninja -C ../native -j"$JOBS" llvm-tblgen clang-tblgen
  # 2. the tools for Android.
  env -u CFLAGS -u CXXFLAGS -u LDFLAGS -u CPPFLAGS \
    cmake -S llvm -B ../android "${common[@]}" -DCMAKE_BUILD_TYPE=MinSizeRel \
      -DCMAKE_TOOLCHAIN_FILE="$SQ_NDK/build/cmake/android.toolchain.cmake" \
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM="android-$SQ_API" -DANDROID_STL=c++_static \
      -DCMAKE_INSTALL_PREFIX="$SQ_PREFIX" \
      -DLLVM_ENABLE_PROJECTS="clang;clang-tools-extra" \
      -DLLVM_HOST_TRIPLE="aarch64-unknown-linux-android$SQ_API" \
      -DLLVM_DEFAULT_TARGET_TRIPLE="aarch64-unknown-linux-android$SQ_API" \
      -DLLVM_NATIVE_TOOL_DIR="$PWD/../native/bin" \
      -DCROSS_TOOLCHAIN_FLAGS_NATIVE="-DCMAKE_C_COMPILER=${CC_FOR_BUILD:-cc};-DCMAKE_CXX_COMPILER=${CXX_FOR_BUILD:-c++}" \
      -DLLVM_ENABLE_PLUGINS=OFF -DCLANG_PLUGIN_SUPPORT=OFF -DCLANG_BUILD_EXAMPLES=OFF \
      -DLLVM_APPEND_VC_REV=OFF -DLLVM_INSTALL_TOOLCHAIN_ONLY=ON \
      -DCMAKE_EXE_LINKER_FLAGS="-Wl,-z,max-page-size=16384" \
      -DLLVM_DISTRIBUTION_COMPONENTS="$components"
  ninja -C ../android -j"$JOBS" distribution
  DESTDIR="$dest" ninja -C ../android install-distribution
}

recipe_after() {
  mkdir -p share/licenses/llvm-tools
  cp "$src/llvm/LICENSE.TXT" share/licenses/llvm-tools/LICENSE.TXT
  # clang-doc's assets, when its install has none, still a folder to keep.
  # clangd finds clang's builtin headers in lib/clang/23, which llvm carries.
  mkdir -p share/clang-doc
}
