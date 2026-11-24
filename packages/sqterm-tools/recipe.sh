# sqterm-tools: the sqterm-* commands (sq_app/cli/sqterm_cli.cpp,
# docs/install-api.md), sqterm's own: clients of the API the app serves at
# $PREFIX/var/run/sqterm.sock (sq_android/api.cpp). One program, named for
# what it does:
#
#   bin/sqterm-api      one request as given, its response (for testing)
#   bin/sqterm-launch   start an installed app: a link to sqterm-api
#   bin/sqterm-install  install an APK (Android asks): a link
#   bin/sqterm-uninstall  uninstall an app (Android asks): a link
#   bin/sqls            ls with Nerd Font icons and LS_COLORS colours
#                       (sq_app/cli/sqls.cpp; zshrc aliases ls to it)
#
# Built from this repository (no source to download), as pkg is: by the
# SDK's toolchain (squared/build/toolchain.sh's wrappers), against the
# sysroot and runtimes packages; yyjson (squared's copy) for the JSON.

version=3
license="sqterm's own"
file=
top=
depends="sysroot runtimes"
inputs="sq_app/cli/sqterm_cli.cpp sq_app/cli/sqls.cpp squared/third_party/yyjson/yyjson.c squared/third_party/yyjson/yyjson.h
        squared/build/toolchain.sh"
keep="bin/sqterm-api bin/sqterm-launch bin/sqterm-install bin/sqterm-uninstall bin/sqls"

recipe_build() {
  local here=$PWD out=$dest$SQ_PREFIX/bin cxx cc ldflags=
  if [ -n "$SQ_HOST_TRIPLE" ]; then
    local work=${dest%/dest}/..
    export SQ_LLVM_BIN=${LLVM_BIN:-/usr/lib/llvm-23/bin} SQ_API
    export SQ_SYSROOT=$work/sysroot/dest$SQ_PREFIX/share/sysroot SQ_RUNTIMES=$work/runtimes/dest$SQ_PREFIX
    bash "$ROOT/squared/build/toolchain.sh" wrappers "$here/toolchain"
    cxx=$here/toolchain/clang++ cc=$here/toolchain/clang
    local key value
    while IFS='=' read -r key value; do
      [ "$key" = SQ_TOOLCHAIN_LDFLAGS ] && ldflags=$value
    done < <(bash "$ROOT/squared/build/toolchain.sh" env)
  else
    # sqterm's host harness: this machine's clang and libc++ ($CXX and $CC
    # may carry flags: wrappers, as pkg's recipe makes them).
    printf '#!/bin/sh\nexec %s "$@"\n' "${CXX:-clang++}" > "$here/clang++"
    printf '#!/bin/sh\nexec %s "$@"\n' "${CC:-clang}" > "$here/clang"
    chmod +x "$here/clang++" "$here/clang"
    cxx=$here/clang++ cc=$here/clang
    ldflags="-stdlib=libc++"
  fi
  local yy=$ROOT/squared/third_party/yyjson
  "$cc" -O2 -w -c "$yy/yyjson.c" -o yyjson.o
  mkdir -p "$out"
  "$cxx" -std=c++23 -stdlib=libc++ -O2 -Wall -Wextra -I"$yy" "$ROOT/sq_app/cli/sqterm_cli.cpp" yyjson.o \
    $ldflags -o "$out/sqterm-api"
  local link
  for link in sqterm-launch sqterm-install sqterm-uninstall; do ln -sf sqterm-api "$out/$link"; done
  "$cxx" -std=c++23 -stdlib=libc++ -O2 -Wall -Wextra "$ROOT/sq_app/cli/sqls.cpp" $ldflags -o "$out/sqls"
}
