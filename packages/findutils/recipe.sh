# findutils 4.10.0: GNU, GPLv3 (its licence travels in the package). Its sources publish
# no sha256: the first download is recorded in packages/sources.lock.

version=4.10.0
file=findutils-$version.tar.xz
url=https://ftp.gnu.org/gnu/findutils/$file
mirrors="https://ftpmirror.gnu.org/gnu/findutils/$file"
sha256=
top=findutils-$version
depends=""
keep="bin/find bin/xargs share/licenses/findutils"
# find and xargs; locate and updatedb left out. tzalloc=no: bionic has
# tzalloc and mktime_z from API 35 only; gnulib then uses its own.
recipe_build() {
  ./configure ${SQ_HOST_TRIPLE:+--host=$SQ_HOST_TRIPLE --build=$(sh ./build-aux/config.guess 2>/dev/null || sh ./support/config.guess)} \
    --prefix="$SQ_PREFIX" --disable-nls --without-selinux ac_cv_func_tzalloc=no
  make -j"$JOBS"
  make install DESTDIR="$dest"
}

recipe_after() {
  mkdir -p share/licenses/findutils
  cp "$src/COPYING" share/licenses/findutils/COPYING
}
