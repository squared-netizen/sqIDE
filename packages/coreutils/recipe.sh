# coreutils 9.8: GNU, GPLv3 (its licence travels in the package). Its sources publish
# no sha256: the first download is recorded in packages/sources.lock.

version=9.8
file=coreutils-$version.tar.xz
url=https://ftp.gnu.org/gnu/coreutils/$file
mirrors="https://ftpmirror.gnu.org/gnu/coreutils/$file"
sha256=
top=coreutils-$version
depends=""
keep="bin share/licenses/coreutils"
# One binary, every command a symbolic link to it (RAM and size; Android
# refuses hard links). pinky, users, who, uptime: no utmp on Android; stdbuf
# needs a preloaded library.
recipe_build() {
  ./configure ${SQ_HOST_TRIPLE:+--host=$SQ_HOST_TRIPLE --build=$(sh ./build-aux/config.guess 2>/dev/null || sh ./support/config.guess)} \
    --prefix="$SQ_PREFIX" --disable-nls --disable-xattr --disable-acl --without-selinux \
    --without-openssl --without-gmp --enable-single-binary=symlinks \
    --enable-no-install-program=pinky,users,who,uptime,stdbuf \
    gl_cv_host_operating_system=Android
  make -j"$JOBS"
  make install DESTDIR="$dest"
}

recipe_after() {
  mkdir -p share/licenses/coreutils
  cp "$src/COPYING" share/licenses/coreutils/COPYING
}
