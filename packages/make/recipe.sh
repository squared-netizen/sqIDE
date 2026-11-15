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
# Recipes run under /system/bin/sh: Android has no /bin/sh. job.c calls
# confstr (_CS_PATH) unguarded, and bionic has neither: a small one that
# gives Android's default PATH.
recipe_patch() {
  awk 'NR == 1, /^#include "makeint.h"/ { print; if (/^#include "makeint.h"/) {
         print "#if defined(__ANDROID__) && !defined(_CS_PATH)"
         print "#define _CS_PATH 0"
         print "static size_t confstr (int name, char *buf, size_t len)"
         print "{ static const char path[] = \"/system/bin\"; (void) name;"
         print "  if (buf != NULL && len > 0) snprintf (buf, len, \"%s\", path);"
         print "  return sizeof path; }"
         print "#endif" } next } { print }' src/job.c > src/job.c.new
  mv src/job.c.new src/job.c
  grep -q 'define _CS_PATH 0' src/job.c
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
