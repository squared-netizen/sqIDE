# runtimes: what clang's programs link with in sqterm's prefix, LLVM 23
# (step 3): compiler-rt's builtins, libunwind, libc++abi and libc++, with
# libc++'s module sources for `import std`. Static libraries only: each
# program carries what it uses (as squared links libc++ into an app), and
# nothing here is loaded at run time.
#
#   lib/clang/23/lib/aarch64-unknown-linux-android28/libclang_rt.builtins.a
#   lib/aarch64-unknown-linux-android28/libc++.a (libc++abi inside), libunwind.a
#   include/c++/v1, include/aarch64-unknown-linux-android28/c++/v1/__config_site
#   share/libc++/v1/std.cppm, std.compat.cppm and the rest; the manifest
#   clang -print-library-module-manifest-path names
#
# Built from the same LLVM 23 sources as clang, against the sysroot package
# (AOSP's bionic, not the NDK), by a clang 23 for the build machine
# (LLVM_BIN, default /usr/lib/llvm-23/bin: apt.llvm.org's on CI): libc++ 23
# needs a recent clang, and the NDK's is 21. Two CMake runs: the builtins,
# then the runtimes. Nothing links while they build (static libraries,
# CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY), so neither needs the other
# first. With nothing linked, CMake's library checks pass for libraries
# bionic has not (pthread, rt, atomic: in libc, or none), and the objects
# would ask for them: they are told there are none. The recipe ends by
# linking a C++ program and an `import std` one.

version=23.1.5
license="Apache-2.0 WITH LLVM-exception"
file=llvm-project-$version.src.tar.xz
url=https://github.com/llvm/llvm-project/releases/download/llvmorg-$version/$file
mirrors=
sha256=
top=llvm-project-$version.src
depends=sysroot
bundled=no
keep="lib include share/libc++ share/licenses/runtimes"

recipe_build() {
  local bin=${LLVM_BIN:-/usr/lib/llvm-23/bin}
  [ -x "$bin/clang" ] || { echo "no clang 23 at $bin (set LLVM_BIN)" >&2; return 1; }
  local triple=aarch64-unknown-linux-android$SQ_API
  local sysroot
  sysroot=$(cd "$src/../.." && pwd)/sysroot/dest$SQ_PREFIX/share/sysroot
  [ -d "$sysroot/usr/include" ] || { echo "no sysroot at $sysroot (build sysroot first)" >&2; return 1; }
  local major=${version%%.*}
  local -a launch=()
  command -v ccache >/dev/null && launch=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
  local -a cross=(-G Ninja -DCMAKE_BUILD_TYPE=Release
                  -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64
                  -DCMAKE_C_COMPILER="$bin/clang" -DCMAKE_CXX_COMPILER="$bin/clang++"
                  -DCMAKE_ASM_COMPILER="$bin/clang"
                  -DCMAKE_C_COMPILER_TARGET="$triple" -DCMAKE_CXX_COMPILER_TARGET="$triple"
                  -DCMAKE_ASM_COMPILER_TARGET="$triple"
                  -DCMAKE_SYSROOT="$sysroot" -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY
                  -DCMAKE_AR="$bin/llvm-ar" -DCMAKE_RANLIB="$bin/llvm-ranlib"
                  -DCMAKE_C_FLAGS="-fPIC" -DCMAKE_CXX_FLAGS="-fPIC"
                  -DLLVM_ENABLE_PER_TARGET_RUNTIME_DIR=ON "${launch[@]}")

  # 1. compiler-rt's builtins, into clang's resource directory.
  env -u CFLAGS -u CXXFLAGS -u LDFLAGS -u CPPFLAGS \
    cmake -S compiler-rt/lib/builtins -B ../builtins "${cross[@]}" \
      -DCMAKE_INSTALL_PREFIX="$SQ_PREFIX/lib/clang/$major" \
      -DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON -DCOMPILER_RT_BAREMETAL_BUILD=OFF
  ninja -C ../builtins -j"$JOBS"
  DESTDIR="$dest" ninja -C ../builtins install

  # 2. libunwind, libc++abi and libc++ (libc++abi merged into libc++.a),
  #    static, with libc++'s modules installed.
  env -u CFLAGS -u CXXFLAGS -u LDFLAGS -u CPPFLAGS \
    cmake -S runtimes -B ../runtimes "${cross[@]}" \
      -DCMAKE_INSTALL_PREFIX="$SQ_PREFIX" \
      -DLLVM_ENABLE_RUNTIMES="libunwind;libcxxabi;libcxx" \
      -DLLVM_DEFAULT_TARGET_TRIPLE="$triple" \
      -DLIBUNWIND_ENABLE_SHARED=OFF -DLIBCXXABI_ENABLE_SHARED=OFF -DLIBCXX_ENABLE_SHARED=OFF \
      -DLIBCXXABI_USE_LLVM_UNWINDER=ON -DLIBCXXABI_ENABLE_STATIC_UNWINDER=ON \
      -DLIBCXX_ENABLE_STATIC_ABI_LIBRARY=ON -DLIBCXX_INSTALL_MODULES=ON \
      -DLIBCXX_INCLUDE_TESTS=OFF -DLIBCXXABI_INCLUDE_TESTS=OFF -DLIBUNWIND_INCLUDE_TESTS=OFF \
      -DLIBCXX_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_TESTS=OFF \
      -DLIBUNWIND_HAS_PTHREAD_LIB=OFF -DLIBCXXABI_HAS_PTHREAD_LIB=OFF -DLIBCXX_HAS_PTHREAD_LIB=OFF \
      -DLIBCXX_HAS_RT_LIB=OFF -DLIBCXX_HAS_ATOMIC_LIB=OFF
  ninja -C ../runtimes -j"$JOBS"
  DESTDIR="$dest" ninja -C ../runtimes install

  # The proof: a C++ program and an `import std` one link against what was
  # just built and the sysroot (a resource directory for the test: the build
  # machine clang's own headers, these builtins).
  local here_prefix=$dest$SQ_PREFIX rd=$PWD/../test-resource-dir
  rm -rf "$rd" && mkdir -p "$rd"
  cp -R "$("$bin/clang" -print-resource-dir)/include" "$rd/"
  cp -R "$here_prefix/lib/clang/$major/lib" "$rd/"
  # -D__BIONIC_CTYPE_INLINE=inline, as the sysroot's clang.cfg gives every
  # compile in sqterm: import std cannot export bionic's static inline ctype.
  local -a cxx=("$bin/clang++" --target="$triple" --sysroot="$sysroot" -resource-dir="$rd"
                -nostdinc++ -isystem "$here_prefix/include/$triple/c++/v1"
                -isystem "$here_prefix/include/c++/v1" -std=c++23 -fuse-ld=lld
                -D__BIONIC_CTYPE_INLINE=inline
                -rtlib=compiler-rt -unwindlib=libunwind -stdlib=libc++ -static-libstdc++
                -L"$here_prefix/lib/$triple")
  printf '#include <cstdio>\n#include <string>\nint main() { std::string s = "hello"; std::puts(s.c_str()); }\n' > ../hello.cpp
  "${cxx[@]}" ../hello.cpp -o ../hello
  # import std: the module built from the installed sources, then used.
  local std=$here_prefix/share/libc++/v$(ls "$here_prefix/share/libc++" | sed -n 's/^v//p' | head -n 1)
  "${cxx[@]}" -Wno-reserved-module-identifier --precompile "$std/std.cppm" -o ../std.pcm
  "${cxx[@]}" -c ../std.pcm -o ../std.o
  printf 'import std;\nint main() { std::println("hello from import std"); }\n' > ../hello_std.cpp
  "${cxx[@]}" -fmodule-file=std=../std.pcm ../hello_std.cpp ../std.o -o ../hello_std
  "$bin/llvm-readelf" -d ../hello_std | grep 'NEEDED'
  echo "runtimes: hello and import std link"
}

recipe_after() {
  mkdir -p share/licenses/runtimes
  cp "$src/libcxx/LICENSE.TXT" share/licenses/runtimes/LICENSE.TXT
}
