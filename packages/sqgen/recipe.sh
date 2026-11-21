# sqgen: squared's layout compiler (squared/tools/sqgen), MIT. An app's
# sq_app/names.json and sq_app/layouts/*.json into C++ (names.hpp,
# layouts.hpp, layouts.cpp), which template.android.cpp's build
# (sq/build/template.lua) runs for an app that has them.
#
#   bin/sqgen      one static program (libc++ linked in; only Android's
#                  libc, libm and libdl at run time)
#
# Built with sqmake, as an app is: squared's build/squared.lua (its
# libraries, platform "host": no Android-only sources, the null graphics
# backend) and the SDK's toolchain (build/toolchain.lua), from the squared
# sqterm carries (SQUARED_REV says which; its key follows it). The source
# fetched is sqmake's, at the tag sources.lock records as sqgen@: built
# first for the build machine (clang 23 and libc++ 23, LLVM_BIN), it builds
# sqgen for Android.

version=0.1.0
license=MIT
git=https://github.com/squared-netizen/sqmake
tag=v0.6.0
file=
top=
depends="sysroot runtimes"
# squared changes only by tools/vendor_framework.sh, which writes SQUARED_REV.
inputs="SQUARED_REV squared/build/squared.lua squared/build/toolchain.lua squared/tools/sqgen/sqgen.cpp"
bundled=yes
keep="bin/sqgen share/licenses/sqgen"

recipe_build() {
  local bin=${LLVM_BIN:-/usr/lib/llvm-23/bin}
  [ -x "$bin/clang++" ] || { echo "no clang 23 at $bin (set LLVM_BIN)" >&2; return 1; }
  local here=${dest%/dest}
  local sysroot=$here/../sysroot/dest$SQ_PREFIX/share/sysroot rt=$here/../runtimes/dest$SQ_PREFIX
  [ -d "$sysroot/usr/include" ] || { echo "no sysroot at $sysroot (build sysroot first)" >&2; return 1; }
  # 1. sqmake, for this machine: its own Makefile and CI's way (clang 23,
  #    libc++ 23), with none of the Android flags build.sh exports.
  #    CC too: build.sh's is the Android one (yyjson is C).
  env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u CC -u CXX make -C "$src" -j"$JOBS" BUILD="$here/host" \
    CXX="$bin/clang++" CC="$bin/clang" AR="$bin/llvm-ar" "$here/host/sqmake"
  # 2. sqgen, for Android: a build.lua beside a copy of squared (sqmake
  #    writes its build/ there), the toolchain found from the installs of
  #    what this depends on.
  local work=$here/work
  rm -rf "$work" && mkdir -p "$work"
  cp -R "$ROOT/squared" "$work/squared"
  cat > "$work/build.lua" <<'LUA'
toolchain.sdk("squared/build/toolchain.lua")
squared_settings = {root = "squared", platform = "host", graphics = "null"}
local squared = include("squared/build/squared.lua")
exe{name = "sqgen", srcs = {"squared/tools/sqgen/sqgen.cpp"}, deps = {squared.gui, squared.messaging}, flags = {"-O2"}}
LUA
  ( cd "$work" &&
    env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u CXX -u CC \
      SQ_LLVM_BIN="$bin" SQ_SYSROOT="$sysroot" SQ_RUNTIMES="$rt" SQ_API="$SQ_API" \
      "$here/host/sqmake" -j"$JOBS" )
  mkdir -p "$dest$SQ_PREFIX/bin"
  cp "$work/build/sqgen" "$dest$SQ_PREFIX/bin/sqgen"
  # What it is: an arm64 Android program that needs no C++ library.
  "$bin/llvm-readelf" -h "$dest$SQ_PREFIX/bin/sqgen" | "$SQEDIT" match -q -F AArch64
  "$bin/llvm-readelf" -l "$dest$SQ_PREFIX/bin/sqgen" | "$SQEDIT" match -q -F /system/bin/linker64
  ! "$bin/llvm-readelf" -d "$dest$SQ_PREFIX/bin/sqgen" | "$SQEDIT" match -q '"NEEDED" (!"c++" .)* "c++"'
}

recipe_after() {
  local to=share/licenses/sqgen
  mkdir -p "$to"
  # squared's (sqterm's LICENSE is the same text; squared/ carries none).
  cp "$ROOT/LICENSE" "$to/LICENSE"
  cp "$ROOT/squared/third_party/yyjson/LICENSE" "$to/yyjson.LICENSE"
  # stb_image's, the end of its header.
  tail -n 41 "$ROOT/squared/third_party/stb/stb_image.h" > "$to/stb_image.LICENSE"
}
