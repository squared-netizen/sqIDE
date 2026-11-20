# zlib 1.3.1: compression, the zlib licence. A static library and its
# headers: git links it in (and anything built in sqterm may). No program.

version=1.3.1
license=Zlib
file=zlib-$version.tar.gz
url=https://zlib.net/fossils/$file
mirrors="https://github.com/madler/zlib/releases/download/v$version/$file"
sha256=
top=zlib-$version
depends=
bundled=no
keep="include/zlib.h include/zconf.h lib/libz.a lib/pkgconfig/zlib.pc share/licenses/zlib"

recipe_build() {
  # zlib's own configure: no --host; the compiler comes from CC.
  CHOST=${SQ_HOST_TRIPLE:-} ./configure --prefix="$SQ_PREFIX" --static
  make -j"$JOBS" libz.a
  make install DESTDIR="$dest"
}

recipe_after() {
  mkdir -p share/licenses/zlib
  cp "$src/LICENSE" share/licenses/zlib/LICENSE
}
