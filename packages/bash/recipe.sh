# bash 5.3: GNU, GPLv3 (its licence travels in the package). Its sources publish
# no sha256: the first download is recorded in packages/sources.lock.

version=5.3
license=GPL-3.0-or-later
file=bash-$version.tar.gz
url=https://ftp.gnu.org/gnu/bash/$file
mirrors="https://ftpmirror.gnu.org/gnu/bash/$file"
sha256=
top=bash-$version
depends="ncurses"
keep="bin/bash share/licenses/bash"
# Readline draws through ncurses (linked in, from the ncurses recipe). The
# cache values are what configure cannot test in a cross build: bionic's.
# Not faccessat: bionic's refuses AT_EACCESS (EINVAL), so every [ -x ], -r
# and -w came out false (on the phone, 2026-11-21: toolchain.sh found no
# clang++); without it bash uses access(), or stat when the ids differ.
recipe_build() {
  ./configure ${SQ_HOST_TRIPLE:+--host=$SQ_HOST_TRIPLE --build=$(sh ./build-aux/config.guess 2>/dev/null || sh ./support/config.guess)} \
    --prefix="$SQ_PREFIX" --disable-nls --without-bash-malloc --enable-multibyte --with-curses \
    bash_cv_termcap_lib=libncursesw bash_cv_getcwd_malloc=yes bash_cv_job_control_missing=present \
    bash_cv_sys_siglist=yes bash_cv_func_sigsetjmp=present bash_cv_unusable_rtsigs=no \
    bash_cv_dev_fd=standard bash_cv_dev_stdin=present bash_cv_func_strcoll_broken=no \
    bash_cv_printf_a_format=yes bash_cv_wcwidth_broken=no bash_cv_func_ctype_nonascii=no \
    ac_cv_func_faccessat=no
  make -j"$JOBS"
  make install DESTDIR="$dest"
}

recipe_after() {
  mkdir -p share/licenses/bash
  cp "$src/COPYING" share/licenses/bash/COPYING
}
