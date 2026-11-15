# pkg: sqterm's package tool (sq_app/pkg/pkg.cpp): list, files, install from
# a file (--consume), remove, backup. sqterm's own, built from this
# repository: no source to download. Statically linked with the C++ library
# (libc++ from the NDK on Android): one file in bin/.

version=1
license="sqterm's own"
file=
top=
depends=
# What it is built from: these decide its key.
inputs="sq_app/pkg/pkg.cpp sq_app/src/unpack.cpp sq_app/src/unpack.hpp
        sq_app/third_party/zstd/zstddeclib.c sq_app/third_party/zstd/zstd.h
        squared/third_party/yyjson/yyjson.c squared/third_party/yyjson/yyjson.h"
keep="bin/pkg"

recipe_build() {
  local cxx=${CC/clang/clang++} out=$dest$SQ_PREFIX/bin
  local -a cxxflags=(-std=c++23 -O2 -Wall -Wextra)
  # Termux's clang++ looks for libc++'s headers beside itself; --sysroot
  # hides them: the NDK's (as the app's build takes them).
  if [ -n "${SQ_SYSROOT:-}" ]; then
    cxxflags+=(-stdlib++-isystem "$SQ_SYSROOT/usr/include/c++/v1" -static-libstdc++)
  fi
  [ -n "$SQ_HOST_TRIPLE" ] || cxx=${CXX:-c++}
  mkdir -p "$out"
  $CC $CFLAGS -c "$ROOT/sq_app/third_party/zstd/zstddeclib.c" -o zstd.o
  $CC $CFLAGS -c "$ROOT/squared/third_party/yyjson/yyjson.c" -o yyjson.o
  $cxx $CFLAGS "${cxxflags[@]}" -I"$ROOT/sq_app/src" -I"$ROOT/sq_app/third_party/zstd" \
    -I"$ROOT/squared/third_party/yyjson" \
    "$ROOT/sq_app/pkg/pkg.cpp" "$ROOT/sq_app/src/unpack.cpp" zstd.o yyjson.o \
    $LDFLAGS -o "$out/pkg"
}
