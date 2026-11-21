# lldb: the debugger for sqterm's prefix, LLVM 23 (step 3, the last of the
# clang stack).
#
#   bin/lldb             the debugger
#   bin/lldb-server      what lldb starts to run and stop a program (on
#                        Linux and Android lldb debugs through it, locally too)
#   bin/lldb-dap         the Debug Adapter Protocol server, for an editor
#                        (neovim's nvim-dap)
#   bin/lldb-argdumper   expands a program's arguments as the shell would
#   lib/liblldb.so       lldb itself; the programs above are front ends
#                        to it (no version in its name: Android has none)
#
# lldb's core is a shared library, and a program and its shared library
# must share one C++ library: here the runtimes package's libc++_shared.so.
# So unlike llvm and llvm-tools (the NDK's CMake toolchain, its libc++
# linked in) lldb is built as the runtimes are: a clang 23 for the build
# machine (LLVM_BIN, default /usr/lib/llvm-23/bin: apt.llvm.org's on CI),
# against the sysroot package and the runtimes' headers and libraries, no
# NDK. CMake is told the system is Android (lldb's Host layer has Android
# sources of its own) with CMAKE_SYSTEM_VERSION=1, which makes CMake skip
# looking for an NDK and take the compiler and sysroot as given.
#
# The programs and liblldb.so find liblldb.so and libc++_shared.so through
# a RUNPATH of $SQ_PREFIX/lib, given at the link: for Android CMake writes
# none (the first build here, 2026-11-17, had no RUNPATH at all), and every
# package is built for this one prefix. Expressions use clang's builtin headers,
# lib/clang/23, which llvm carries.
#
# Not yet: Python and Lua scripting, libedit (line editing and history at
# the (lldb) prompt) and the curses GUI. Each is a library to build first.
# lldb keeps temporary files in $TMPDIR, which sqterm sets (Android's
# default, /data/local/tmp, is not an app's to write).
#
# Large: not in the APK (bundled=no). tools/fetch_packages.sh --get lldb;
# in sqterm, pkg install it --consume, with runtimes installed.

version=23.1.5
license="Apache-2.0 WITH LLVM-exception"
file=llvm-project-$version.src.tar.xz
url=https://github.com/llvm/llvm-project/releases/download/llvmorg-$version/$file
mirrors=
sha256=
top=llvm-project-$version.src
depends="sysroot runtimes"
bundled=no
keep="bin lib/liblldb.so share/licenses/lldb"

components="lldb;liblldb;lldb-server;lldb-dap;lldb-argdumper"

recipe_patch() {
  # Before it starts a program to debug, lldb-server's child calls
  # setgid(getgid()); Android's seccomp filter for apps kills a process that
  # calls setgid, so the child died before exec and lldb said "could not
  # sync with inferior process" (2026-11-17, on the phone). An app has no
  # setgid powers to drop. Termux's lldb carries the same change.
  local f=lldb/source/Host/posix/ProcessLauncherPosixFork.cpp
  "$SQEDIT" wrap -x -F --lines 2 --in-place --expect 1 \
    '    if (setgid(getgid()) != 0)' '#if !defined(__ANDROID__)' '#endif' "$f"
}

recipe_build() {
  local bin=${LLVM_BIN:-/usr/lib/llvm-23/bin}
  [ -x "$bin/clang" ] || { echo "no clang 23 at $bin (set LLVM_BIN)" >&2; return 1; }
  local triple=aarch64-unknown-linux-android$SQ_API
  local work sysroot rt
  work=$(cd "$src/../.." && pwd)
  sysroot=$work/sysroot/dest$SQ_PREFIX/share/sysroot
  rt=$work/runtimes/dest$SQ_PREFIX
  [ -d "$sysroot/usr/include" ] || { echo "no sysroot at $sysroot (build sysroot first)" >&2; return 1; }
  [ -e "$rt/lib/$triple/libc++_shared.so" ] || { echo "no libc++_shared.so in $rt (build runtimes first)" >&2; return 1; }
  local major=${version%%.*}

  # A resource directory: the build machine clang's own headers, the
  # runtimes' builtins for Android.
  local rd=$PWD/../resource-dir
  rm -rf "$rd" && mkdir -p "$rd"
  cp -R "$("$bin/clang" -print-resource-dir)/include" "$rd/"
  cp -R "$rt/lib/clang/$major/lib" "$rd/"

  local -a launch=()
  command -v ccache >/dev/null && launch=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
  local common=(-G Ninja -DLLVM_TARGETS_TO_BUILD=AArch64
                -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF
                -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_DOCS=OFF -DLLDB_INCLUDE_TESTS=OFF
                -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF
                -DLLVM_ENABLE_LIBEDIT=OFF -DLLVM_ENABLE_LIBPFM=OFF
                -DLLDB_ENABLE_PYTHON=OFF -DLLDB_ENABLE_LUA=OFF -DLLDB_ENABLE_SWIG=OFF
                -DLLDB_ENABLE_LIBEDIT=OFF -DLLDB_ENABLE_CURSES=OFF -DLLDB_ENABLE_LZMA=OFF
                -DLLDB_ENABLE_LIBXML2=OFF -DLLDB_ENABLE_TREESITTER=OFF
                "${launch[@]}")

  # 1. tablegen for this machine (it runs while LLVM builds), lldb's too.
  env -u CFLAGS -u CXXFLAGS -u LDFLAGS -u CPPFLAGS \
    cmake -S llvm -B ../native "${common[@]}" -DCMAKE_BUILD_TYPE=Release \
      -DLLVM_ENABLE_PROJECTS="clang;lldb" \
      -DCMAKE_C_COMPILER="${CC_FOR_BUILD:-cc}" -DCMAKE_CXX_COMPILER="${CXX_FOR_BUILD:-c++}"
  ninja -C ../native -j"$JOBS" llvm-tblgen clang-tblgen lldb-tblgen

  # 2. lldb for Android, against the sysroot and libc++_shared.so.
  local cflags="-fPIC -resource-dir=$rd"
  local cxxflags="$cflags -nostdinc++ -isystem $rt/include/$triple/c++/v1 -isystem $rt/include/c++/v1"
  local ldflags="-fuse-ld=lld -rtlib=compiler-rt -unwindlib=libunwind -resource-dir=$rd -nostdlib++ -L$rt/lib/$triple -Wl,-z,max-page-size=16384 -Wl,-rpath,$SQ_PREFIX/lib"
  env -u CFLAGS -u CXXFLAGS -u LDFLAGS -u CPPFLAGS \
    cmake -S llvm -B ../android "${common[@]}" -DCMAKE_BUILD_TYPE=MinSizeRel \
      -DCMAKE_SYSTEM_NAME=Android -DCMAKE_SYSTEM_VERSION=1 -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
      -DCMAKE_C_COMPILER="$bin/clang" -DCMAKE_CXX_COMPILER="$bin/clang++" -DCMAKE_ASM_COMPILER="$bin/clang" \
      -DCMAKE_C_COMPILER_TARGET="$triple" -DCMAKE_CXX_COMPILER_TARGET="$triple" \
      -DCMAKE_ASM_COMPILER_TARGET="$triple" \
      -DCMAKE_SYSROOT="$sysroot" -DCMAKE_FIND_ROOT_PATH="$sysroot" \
      -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
      -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY \
      -DCMAKE_AR="$bin/llvm-ar" -DCMAKE_RANLIB="$bin/llvm-ranlib" \
      -DCMAKE_C_FLAGS="$cflags" -DCMAKE_CXX_FLAGS="$cxxflags" -DCMAKE_ASM_FLAGS="$cflags" \
      -DCMAKE_EXE_LINKER_FLAGS="$ldflags" -DCMAKE_SHARED_LINKER_FLAGS="$ldflags" \
      -DCMAKE_MODULE_LINKER_FLAGS="$ldflags" -DCMAKE_CXX_STANDARD_LIBRARIES="-lc++_shared" \
      -DCMAKE_INSTALL_PREFIX="$SQ_PREFIX" \
      -DLLVM_ENABLE_PROJECTS="clang;lldb" \
      -DLLVM_HOST_TRIPLE="$triple" -DLLVM_DEFAULT_TARGET_TRIPLE="$triple" \
      -DLLVM_NATIVE_TOOL_DIR="$PWD/../native/bin" \
      -DCROSS_TOOLCHAIN_FLAGS_NATIVE="-DCMAKE_C_COMPILER=${CC_FOR_BUILD:-cc};-DCMAKE_CXX_COMPILER=${CXX_FOR_BUILD:-c++}" \
      -DLLVM_ENABLE_PLUGINS=OFF -DCLANG_PLUGIN_SUPPORT=OFF -DCLANG_BUILD_EXAMPLES=OFF \
      -DLLVM_APPEND_VC_REV=OFF \
      -DLLVM_DISTRIBUTION_COMPONENTS="$components"
  ninja -C ../android -j"$JOBS" distribution
  DESTDIR="$dest" ninja -C ../android install-distribution

  # The proof (nothing here runs arm64 Android): every program and liblldb
  # need libc++_shared.so.1, the programs liblldb, each finds them through
  # its RUNPATH, and nothing asks for a library the phone lacks.
  local p d
  for p in bin/lldb bin/lldb-server bin/lldb-dap bin/lldb-argdumper lib/liblldb.so; do
    d=$("$bin/llvm-readelf" -d "$dest$SQ_PREFIX/$p")
    echo "$p:"; echo "$d" | "$SQEDIT" match '"NEEDED" / "RUNPATH" / "RPATH"'
    echo "$d" | "$SQEDIT" match -q "\"RUNPATH\" (!\"[$SQ_PREFIX/lib]\" .)* \"[$SQ_PREFIX/lib]\"" || [ "$p" = bin/lldb-argdumper ] ||
      { echo "$p has no RUNPATH of $SQ_PREFIX/lib" >&2; return 1; }
    echo "$d" | "$SQEDIT" match -F NEEDED |
      "$SQEDIT" match -v '"[" ("libc++_shared" / "libc" / "libm" / "libdl" / "liblldb") ".so" ".1"? "]"' &&
      { echo "$p needs a library sqterm has not" >&2; return 1; }
  done
  "$bin/llvm-readelf" -d "$dest$SQ_PREFIX/bin/lldb" | "$SQEDIT" match -q -F 'Shared library: [liblldb.so]'
  "$bin/llvm-readelf" -d "$dest$SQ_PREFIX/lib/liblldb.so" | "$SQEDIT" match -q -F 'Shared library: [libc++_shared.so.1]'
  "$bin/llvm-readelf" -d "$dest$SQ_PREFIX/bin/lldb" | "$SQEDIT" match -q "\"RUNPATH\" (!\"[$SQ_PREFIX/lib]\" .)* \"[$SQ_PREFIX/lib]\""
  echo "lldb: links against liblldb and libc++_shared.so.1"
}

recipe_after() {
  mkdir -p share/licenses/lldb
  cp "$src/lldb/LICENSE.TXT" share/licenses/lldb/LICENSE.TXT
}
