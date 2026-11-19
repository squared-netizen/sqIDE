# pkg: sqterm's package tool (sq_app/pkg/pkg.cpp): list, files, update,
# upgrade, search, install (from the release through curl, or from a file),
# remove, backup. sqterm's own, built from this
# repository: no source to download. Statically linked with the C++ library
# (libc++ from the NDK on Android): one file in bin/.
#
# It reads packages that are squared cartridges (.sq, the release's
# index-3) through sqcart's reader (third_party/sqcart: sq.cart, C++23
# modules), and tarballs (.tar.zst) through sq_app/src/unpack.cpp. sq.cart
# is built here as the app builds squared: with the NDK's clang and the std
# module from the NDK's own libc++ sources, as squared pins them
# (squared/third_party/libcxx). sqcart's reading archives carry yyjson and
# zstd's decoder, the same files as squared's and sq_app's copies: pkg
# links them from there, once.

version=1
license="sqterm's own"
file=
top=
depends=
# What it is built from: these decide its key (a PINNED lists its folder's
# sources' sha256s).
inputs="sq_app/pkg/pkg.cpp sq_app/src/unpack.cpp sq_app/src/unpack.hpp sq_app/src/unpack_cartridge.cpp
        sq_app/third_party/zstd/zstd.h squared/third_party/yyjson/yyjson.h
        third_party/sqcart/PINNED squared/third_party/libcxx/PINNED"
keep="bin/pkg"

recipe_build() {
  local out=$dest$SQ_PREFIX/bin here=$PWD
  local -a cxxflags=(-std=c++23 -O2 -Wall -Wextra)
  # Termux's clang++ looks for libc++'s headers beside itself; --sysroot
  # hides them: the NDK's (as the app's build takes them).
  local libcxx=
  if [ -n "${SQ_SYSROOT:-}" ]; then
    libcxx="-stdlib++-isystem $SQ_SYSROOT/usr/include/c++/v1"
    cxxflags+=(-static-libstdc++)
  fi
  # clang and clang++ for this target, each one word (sqcart's Makefile
  # takes one for CXX, and makes CC of it): two scripts.
  # -D__BIONIC_CTYPE_INLINE=inline: the std module exports bionic's ctype
  # functions, which must be plain inline (as squared builds).
  local cxx=$here/clang++
  if [ -n "$SQ_HOST_TRIPLE" ]; then
    printf '#!/bin/sh\nexec %s %s -D__BIONIC_CTYPE_INLINE=inline "$@"\n' "${CC/clang/clang++}" "$libcxx" > "$here/clang++"
    printf '#!/bin/sh\nexec %s "$@"\n' "$CC" > "$here/clang"
  else
    printf '#!/bin/sh\nexec %s "$@"\n' "${CXX:-clang++}" > "$here/clang++"
    printf '#!/bin/sh\nexec %s "$@"\n' "${CC:-clang}" > "$here/clang"
  fi
  chmod +x "$here/clang++" "$here/clang"
  # sq.cart's reading archives: libsqcart.a (sq.cart, sq.data, yyjson,
  # Monocypher, the std module's object) and libsqcart-unzstd.a.
  local cart=$here/sqcart std=$ROOT/squared/third_party/libcxx/std.cppm
  make -C "$ROOT/third_party/sqcart/v3" -j"$JOBS" CXX="$cxx" BUILD="$cart" OPT=-O2 STD_MANIFEST= STD_CPPM="$std" \
    "$cart/libsqcart.a" "$cart/libsqcart-unzstd.a" "$cart/pcm/sq.cart.pcm"
  # As the modules were compiled (no $CFLAGS: clang refuses a module made
  # with other options).
  "$cxx" -std=c++23 -stdlib=libc++ -O2 -Wall -Wextra -fprebuilt-module-path="$cart/pcm" -fmodule-file=std="$cart/pcm/std.pcm" \
    -I"$ROOT/sq_app/src" -c "$ROOT/sq_app/src/unpack_cartridge.cpp" -o unpack_cartridge.o
  mkdir -p "$out"
  "$cxx" $CFLAGS "${cxxflags[@]}" -I"$ROOT/sq_app/src" -I"$ROOT/sq_app/third_party/zstd" \
    -I"$ROOT/squared/third_party/yyjson" \
    "$ROOT/sq_app/pkg/pkg.cpp" "$ROOT/sq_app/src/unpack.cpp" unpack_cartridge.o \
    "$cart/libsqcart.a" "$cart/libsqcart-unzstd.a" $LDFLAGS -o "$out/pkg"
}
