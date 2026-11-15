# sed 4.9: GNU, GPLv3 (its licence travels in the package). Its sources publish
# no sha256: the first download is recorded in packages/sources.lock.

version=4.9
file=sed-$version.tar.xz
url=https://ftp.gnu.org/gnu/sed/$file
mirrors="https://ftpmirror.gnu.org/gnu/sed/$file"
sha256=
top=sed-$version
depends=""
keep="bin/sed share/licenses/sed"

# sed 4.9's gnulib (2022) has a lib/cdefs.h whose __bos clashes with the
# NDK's fortify headers (__USE_FORTIFY_LEVEL undeclared): built without
# _FORTIFY_SOURCE, for this package only.
recipe_build() {
  CPPFLAGS="$CPPFLAGS -U_FORTIFY_SOURCE" ./configure ${SQ_HOST_TRIPLE:+--host=$SQ_HOST_TRIPLE --build=$(sh ./build-aux/config.guess 2>/dev/null || sh ./support/config.guess)} \
    --prefix="$SQ_PREFIX" --disable-nls --disable-acl --without-selinux
  make -j"$JOBS"
  make install DESTDIR="$dest"
}

recipe_after() {
  mkdir -p share/licenses/sed
  cp "$src/COPYING" share/licenses/sed/COPYING
}
