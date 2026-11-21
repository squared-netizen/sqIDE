# sqdrive 0.5.0: the Virtual Cartridge Drive's tool, MIT. Reads and writes
# the files of a squared cartridge (sqcart 3.0) by range: ls, stat, cat,
# bench, write, truncate, rm, mv, commit, recover. Its journals go in
# ~/.local/state/sqdrive (or $SQDRIVE_STAGING).
#
#   bin/sqdrive    one static program (libc++ linked in; only Android's
#                  libc, libm and libdl at run time)
#
# C++23 modules (`import std`), so built as squared's own C++ is: by a clang
# 23 for the build machine (LLVM_BIN, as the runtimes are; the NDK's is 21),
# against the sysroot package (bionic) and the runtimes package (libc++ 23
# and its std.cppm), with sqdrive's own Makefile. sqcart, zstd, yyjson,
# Monocypher and squared's sq.data are vendored in its repository: nothing
# else to download. A git source: the tag's commit is in sources.lock.

version=0.5.0
license=MIT
git=https://github.com/squared-netizen/sqdrive
tag=v$version
file=
top=
depends="sysroot runtimes"
bundled=no
keep="bin/sqdrive share/licenses/sqdrive"

recipe_build() {
  local bin=${LLVM_BIN:-/usr/lib/llvm-23/bin}
  [ -x "$bin/clang++" ] || { echo "no clang 23 at $bin (set LLVM_BIN)" >&2; return 1; }
  local triple=aarch64-unknown-linux-android$SQ_API
  local here=${dest%/dest}
  local sysroot=$here/../sysroot/dest$SQ_PREFIX/share/sysroot rt=$here/../runtimes/dest$SQ_PREFIX
  [ -d "$sysroot/usr/include" ] || { echo "no sysroot at $sysroot (build sysroot first)" >&2; return 1; }
  local std
  std=$(ls "$rt"/share/libc++/v*/std.cppm 2>/dev/null | head -n 1)
  [ -f "$std" ] || { echo "no std.cppm in $rt (build runtimes first)" >&2; return 1; }
  # A resource directory: the build machine clang's own headers, the
  # runtimes' builtins (as the runtimes recipe links its proofs).
  local rd=$here/resource-dir out=$here/obj
  rm -rf "$rd" "$out" && mkdir -p "$rd" "$out"
  cp -R "$("$bin/clang" -print-resource-dir)/include" "$rd/"
  cp -R "$rt"/lib/clang/*/lib "$rd/"
  # clang and clang++ for Android, as two scripts: the Makefiles take one
  # word for CXX (and make CC of it). -D__BIONIC_CTYPE_INLINE=inline, as the
  # sysroot's clang.cfg gives every compile: import std cannot export
  # bionic's static inline ctype. The runtime libraries on every compile,
  # not only the link: they decide target features (compiler-rt turns on
  # outline atomics and function multiversioning), and a module compiled
  # with others is refused (CI, 2026-11-19: apt.llvm.org's clang defaults
  # to libgcc, so std.pcm had -fmv and the link's compile +outline-atomics).
  local tools=$here/tools
  local flags="--target=$triple --sysroot=$sysroot -resource-dir=$rd -rtlib=compiler-rt -unwindlib=libunwind -D__BIONIC_CTYPE_INLINE=inline"
  mkdir -p "$tools"
  printf '#!/bin/sh\nexec "%s" %s "$@"\n' "$bin/clang" "$flags" > "$tools/clang"
  printf '#!/bin/sh\nexec "%s" %s -nostdinc++ -isystem "%s" -isystem "%s" "$@"\n' "$bin/clang++" "$flags" \
    "$rt/include/$triple/c++/v1" "$rt/include/c++/v1" > "$tools/clang++"
  chmod +x "$tools/clang" "$tools/clang++"
  # The clone is read in place: everything is built in $out.
  env -u CFLAGS -u CXXFLAGS -u CPPFLAGS make -j"$JOBS" BUILD="$out" CXX="$tools/clang++" OPT=-O2 STD_MANIFEST= STD_CPPM="$std" \
    LDFLAGS="-stdlib=libc++ -static-libstdc++ -fuse-ld=lld -L$rt/lib/$triple" \
    "$out/sqdrive"
  mkdir -p "$dest$SQ_PREFIX/bin"
  cp "$out/sqdrive" "$dest$SQ_PREFIX/bin/sqdrive"
  # What it is: an arm64 Android program that needs no C++ library.
  "$bin/llvm-readelf" -h "$dest$SQ_PREFIX/bin/sqdrive" | "$SQEDIT" match -q -F AArch64
  "$bin/llvm-readelf" -l "$dest$SQ_PREFIX/bin/sqdrive" | "$SQEDIT" match -q -F /system/bin/linker64
  ! "$bin/llvm-readelf" -d "$dest$SQ_PREFIX/bin/sqdrive" | "$SQEDIT" match -q '"NEEDED" (!"c++" .)* "c++"'
}

recipe_after() {
  local to=share/licenses/sqdrive tp=$src/third_party/sqcart
  mkdir -p "$to"
  cp "$src/LICENSE" "$to/LICENSE"
  cp "$tp/LICENSE" "$to/sqcart.LICENSE"
  cp "$tp/third_party/squared-data/LICENSE" "$to/squared-data.LICENSE"
  cp "$tp/third_party/zstd-1.5.7/LICENSE" "$to/zstd.LICENSE"
  cp "$tp/third_party/yyjson-0.12.0/LICENSE" "$to/yyjson.LICENSE"
  cp "$tp/third_party/monocypher-4.0.3/LICENCE.md" "$to/monocypher.LICENCE.md"
}
