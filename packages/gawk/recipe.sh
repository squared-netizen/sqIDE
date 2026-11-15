# gawk 5.3.2: GNU, GPLv3 (its licence travels in the package). Its sources publish
# no sha256: the first download is recorded in packages/sources.lock.

version=5.3.2
license=GPL-3.0-or-later
file=gawk-$version.tar.xz
url=https://ftp.gnu.org/gnu/gawk/$file
mirrors="https://ftpmirror.gnu.org/gnu/gawk/$file"
sha256=
top=gawk-$version
depends=""
keep="bin/gawk bin/awk share/licenses/gawk"
# No loadable extensions (no dlopen of .so files from the prefix). No
# spawn.h: awk.h defines a macro `stack`, which breaks linux/sched.h under it.
recipe_build() {
  ./configure ${SQ_HOST_TRIPLE:+--host=$SQ_HOST_TRIPLE --build=$(sh ./build-aux/config.guess 2>/dev/null || sh ./support/config.guess)} \
    --prefix="$SQ_PREFIX" --disable-nls --disable-extensions --without-readline --without-mpfr ac_cv_header_spawn_h=no
  make -j"$JOBS"
  make install DESTDIR="$dest"
}

recipe_after() {
  # The install hard-links gawk-<version>: gone; awk a symbolic link.
  rm -f bin/gawk-* bin/awk
  ln -s gawk bin/awk
  mkdir -p share/licenses/gawk
  cp "$src/COPYING" share/licenses/gawk/COPYING
}
