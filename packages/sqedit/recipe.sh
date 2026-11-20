# sqedit 0.1.0: a text tool on parsing expression grammars, MIT. What sed,
# grep and awk did in squared's and sqterm's builds and scripts: match,
# pick, lines, sub, wrap and json, each with a count it can be held to.
# squared's build needs it (build/common.mk), so it is in the APK.
#
#   bin/sqedit     one static program (libc++ linked in; only Android's
#                  libc, libm and libdl at run time)
#
# C++23 modules (`import std`), so built as sqdrive is: by a clang 23 for
# the build machine (LLVM_BIN), against the sysroot package (bionic) and the
# runtimes package (libc++ 23 and its std.cppm), with sqedit's own Makefile.
# squared's sq.data and yyjson are vendored in its repository. A git source:
# the tag's commit is in sources.lock.

version=0.1.0
license=MIT
git=https://github.com/squared-netizen/sqedit
tag=v$version
file=
top=
depends="sysroot runtimes"
bundled=yes
keep="bin/sqedit share/licenses/sqedit"

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
  # The resource directory and the two compiler scripts, as sqdrive's
  # recipe makes them (its comments say why each flag is there).
  local rd=$here/resource-dir out=$here/obj tools=$here/tools
  rm -rf "$rd" "$out" && mkdir -p "$rd" "$out" "$tools"
  cp -R "$("$bin/clang" -print-resource-dir)/include" "$rd/"
  cp -R "$rt"/lib/clang/*/lib "$rd/"
  local flags="--target=$triple --sysroot=$sysroot -resource-dir=$rd -rtlib=compiler-rt -unwindlib=libunwind -D__BIONIC_CTYPE_INLINE=inline"
  printf '#!/bin/sh\nexec "%s" %s "$@"\n' "$bin/clang" "$flags" > "$tools/clang"
  printf '#!/bin/sh\nexec "%s" %s -nostdinc++ -isystem "%s" -isystem "%s" "$@"\n' "$bin/clang++" "$flags" \
    "$rt/include/$triple/c++/v1" "$rt/include/c++/v1" > "$tools/clang++"
  chmod +x "$tools/clang" "$tools/clang++"
  env -u CFLAGS -u CXXFLAGS -u CPPFLAGS make -j"$JOBS" BUILD="$out" CXX="$tools/clang++" CC="$tools/clang" \
    AR="$bin/llvm-ar" OPT=-O2 STD_MANIFEST= STD_CPPM="$std" \
    LDFLAGS="-stdlib=libc++ -static-libstdc++ -fuse-ld=lld -L$rt/lib/$triple" \
    "$out/sqedit"
  mkdir -p "$dest$SQ_PREFIX/bin"
  cp "$out/sqedit" "$dest$SQ_PREFIX/bin/sqedit"
  # What it is: an arm64 Android program that needs no C++ library.
  "$bin/llvm-readelf" -h "$dest$SQ_PREFIX/bin/sqedit" | grep AArch64 >/dev/null
  "$bin/llvm-readelf" -l "$dest$SQ_PREFIX/bin/sqedit" | grep /system/bin/linker64 >/dev/null
  ! "$bin/llvm-readelf" -d "$dest$SQ_PREFIX/bin/sqedit" | grep 'NEEDED' | grep -q 'c++'
}

recipe_after() {
  local to=share/licenses/sqedit
  mkdir -p "$to"
  cp "$src/LICENSE" "$to/LICENSE"
  cp "$src/third_party/squared-data/LICENSE" "$to/squared-data.LICENSE"
  cp "$src/third_party/yyjson-0.12.0/LICENSE" "$to/yyjson.LICENSE"
}
