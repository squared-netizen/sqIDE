# pkg: sqterm's package tool (sq_app/pkg/pkg.cpp): list, files, update,
# upgrade, search, install (from the release through curl, or from a file),
# remove, backup. sqterm's own, built from this
# repository: no source to download. One file in bin/, libc++ linked in.
#
# It reads packages that are squared cartridges (.sq, the release's
# index-3) through sqcart's reader (third_party/sqcart: sq.cart, C++23
# modules), and tarballs (.tar.zst) through sq_app/src/unpack.cpp.
#
# Built by the SDK's one toolchain (squared/build/toolchain.sh, "One C++23
# toolchain: transition plan"): clang 23 for the build machine (LLVM_BIN, as
# the runtimes are built), against the sysroot package (bionic) and the
# runtimes package (libc++ 23, its std.cppm), with every flag that changes
# a compiled module on every call. sqcart's reading archives carry yyjson
# and zstd's decoder, the same files as squared's and sq_app's copies: pkg
# links them from there, once.

version=3
license="sqterm's own"
file=
top=
# What it is built against (the toolchain's bionic and libc++); at run time
# it needs nothing (packages/needs).
depends="sysroot runtimes"
# What it is built from: these decide its key (a PINNED lists its folder's
# sources' sha256s).
inputs="sq_app/pkg/pkg.cpp sq_app/src/unpack.cpp sq_app/src/unpack.hpp sq_app/src/unpack_cartridge.cpp
        sq_app/third_party/zstd/zstd.h squared/third_party/yyjson/yyjson.h
        third_party/sqcart/PINNED squared/build/toolchain.sh"
keep="bin/pkg"

recipe_build() {
  local here=$PWD out=$dest$SQ_PREFIX/bin cxx cart=$PWD/sqcart std ldflags=
  if [ -n "$SQ_HOST_TRIPLE" ]; then
    # The toolchain, from the installs of what pkg depends on.
    local work=${dest%/dest}/..
    export SQ_LLVM_BIN=${LLVM_BIN:-/usr/lib/llvm-23/bin} SQ_API
    export SQ_SYSROOT=$work/sysroot/dest$SQ_PREFIX/share/sysroot SQ_RUNTIMES=$work/runtimes/dest$SQ_PREFIX
    bash "$ROOT/squared/build/toolchain.sh" wrappers "$here/toolchain"
    cxx=$here/toolchain/clang++
    local key value
    while IFS='=' read -r key value; do
      case $key in
        SQ_STD_CPPM) std=$value ;;
        SQ_TOOLCHAIN_LDFLAGS) ldflags=$value ;;
      esac
    done < <(bash "$ROOT/squared/build/toolchain.sh" env)
  else
    # sqterm's host harness: this machine's clang and libc++ (23, for
    # import std), the std module its libc++ names.
    printf '#!/bin/sh\nexec %s "$@"\n' "${CXX:-clang++}" > "$here/clang++"
    printf '#!/bin/sh\nexec %s "$@"\n' "${CC:-clang}" > "$here/clang"
    chmod +x "$here/clang++" "$here/clang"
    cxx=$here/clang++
    std=
    ldflags="-stdlib=libc++ -lpthread"
  fi
  # sq.cart's reading archives: libsqcart.a (sq.cart, sq.data, yyjson,
  # Monocypher, the std module's object) and libsqcart-unzstd.a.
  make -C "$ROOT/third_party/sqcart/v3" -j"$JOBS" CXX="$cxx" BUILD="$cart" OPT=-O2 \
    ${std:+STD_MANIFEST= STD_CPPM="$std"} \
    "$cart/libsqcart.a" "$cart/libsqcart-unzstd.a" "$cart/pcm/sq.cart.pcm"
  "$cxx" -std=c++23 -stdlib=libc++ -O2 -Wall -Wextra -fprebuilt-module-path="$cart/pcm" -fmodule-file=std="$cart/pcm/std.pcm" \
    -I"$ROOT/sq_app/src" -c "$ROOT/sq_app/src/unpack_cartridge.cpp" -o unpack_cartridge.o
  mkdir -p "$out"
  "$cxx" -std=c++23 -stdlib=libc++ -O2 -Wall -Wextra -I"$ROOT/sq_app/src" -I"$ROOT/sq_app/third_party/zstd" \
    -I"$ROOT/squared/third_party/yyjson" \
    -DSQTERM_PKG_VERSION="\"$version\"" \
    "$ROOT/sq_app/pkg/pkg.cpp" "$ROOT/sq_app/src/unpack.cpp" unpack_cartridge.o \
    "$cart/libsqcart.a" "$cart/libsqcart-unzstd.a" $ldflags -o "$out/pkg"
}
