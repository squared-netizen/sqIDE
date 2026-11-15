# grep 3.12: GNU, GPLv3 (its licence travels in the package). Its sources publish
# no sha256: the first download is recorded in packages/sources.lock.

version=3.12
file=grep-$version.tar.xz
url=https://ftp.gnu.org/gnu/grep/$file
mirrors="https://ftpmirror.gnu.org/gnu/grep/$file"
sha256=
top=grep-$version
depends=""
keep="bin/grep share/licenses/grep"
# egrep and fgrep are #!/bin/sh scripts (no /bin/sh on Android): left out.
recipe_build() {
  ./configure ${SQ_HOST_TRIPLE:+--host=$SQ_HOST_TRIPLE --build=$(sh ./build-aux/config.guess 2>/dev/null || sh ./support/config.guess)} \
    --prefix="$SQ_PREFIX" --disable-nls --disable-perl-regexp
  make -j"$JOBS"
  make install DESTDIR="$dest"
}

recipe_after() {
  mkdir -p share/licenses/grep
  cp "$src/COPYING" share/licenses/grep/COPYING
}
