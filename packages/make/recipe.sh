# make 4.4.1: GNU, GPLv3 (its licence travels in the package). Its sources publish
# no sha256: the first download is recorded in packages/sources.lock.

version=4.4.1
file=make-$version.tar.gz
url=https://ftp.gnu.org/gnu/make/$file
mirrors="https://ftpmirror.gnu.org/gnu/make/$file"
sha256=
top=make-$version
depends=""
keep="bin/make share/licenses/make"
# Recipes run under /system/bin/sh: Android has no /bin/sh.
recipe_patch() {
  sed -i 's|default_shell = "/bin/sh"|default_shell = "/system/bin/sh"|' src/job.c
  grep -q 'default_shell = "/system/bin/sh"' src/job.c
}
recipe_build() {
  ./configure ${SQ_HOST_TRIPLE:+--host=$SQ_HOST_TRIPLE --build=$(sh ./build-aux/config.guess 2>/dev/null || sh ./support/config.guess)} \
    --prefix="$SQ_PREFIX" --disable-nls --without-guile
  make -j"$JOBS"
  make install DESTDIR="$dest"
}

recipe_after() {
  mkdir -p share/licenses/make
  cp "$src/COPYING" share/licenses/make/COPYING
}
