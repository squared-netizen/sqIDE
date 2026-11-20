# git 2.51.0: version control, GPLv2 (its licence travels in the package).
# HTTPS through libcurl and mbedtls, linked in from the curl and mbedtls
# packages' builds; zlib linked in too. So git needs nothing at run time but
# bash, for the few commands that are still shell scripts.
#
# Built with git's own Makefile and a config.mak (no configure, which cannot
# run its probes for another machine). Left out: gettext (English only), Tcl/
# Tk (gitk, git gui), Perl and Python (git send-email, git p4, git svn), expat
# (pushing over WebDAV). Certificates are libcurl's default, Android's trust
# store (/system/etc/security/cacerts, the curl recipe's --with-ca-path);
# `git config --global http.sslCAPath /apex/com.android.conscrypt/cacerts`
# takes Android 14's updatable one.

version=2.51.0
license=GPL-2.0-only
file=git-$version.tar.xz
url=https://www.kernel.org/pub/software/scm/git/$file
mirrors="https://mirrors.edge.kernel.org/pub/software/scm/git/$file"
sha256=
top=git-$version
depends="zlib mbedtls curl"
bundled=no
keep="bin/git bin/git-shell bin/git-upload-pack bin/git-receive-pack bin/git-upload-archive
      libexec/git-core share/git-core share/licenses/git"

# SHELL_PATH is two things to git's Makefile: the shell its built scripts
# and run-command use on the phone ($SQ_PREFIX/bin/bash), and the shell it
# runs its own build steps with. The build machine has no $SQ_PREFIX, so
# wherever the Makefiles run a script with it (recipe lines, and $(shell)
# calls such as GIT-VERSION-GEN's, read before any rule) they use /bin/sh,
# and make's SHELL is given as /bin/sh too (make SHELL=/bin/sh, below). What
# is baked in (SHELL_PATH_SQ in sed and -D) is left alone.
recipe_patch() {
  sed -i -e 's/\$(SHELL_PATH) /\/bin\/sh /g' \
         -e "s/'\$(SHELL_PATH_SQ)' /\/bin\/sh /g" Makefile *.mak
}

recipe_build() {
  # libcurl's static archive needs mbedtls' after it; CPPFLAGS and LDFLAGS
  # carry the three packages' headers and libraries (build.sh's depends).
  cat > config.mak <<MAK
uname_S = Linux
uname_M = aarch64
uname_O = Android
CC = $CC
AR = $AR
CFLAGS = $CFLAGS $CPPFLAGS -include $ROOT/packages/git/android_compat.h
LDFLAGS = $LDFLAGS
prefix = $SQ_PREFIX
SHELL_PATH = $SQ_PREFIX/bin/bash
NO_GETTEXT = YesPlease
NO_TCLTK = YesPlease
NO_PERL = YesPlease
NO_PYTHON = YesPlease
NO_EXPAT = YesPlease
NO_OPENSSL = YesPlease
NO_INSTALL_HARDLINKS = YesPlease
INSTALL_SYMLINKS = YesPlease
NO_REGEX = NeedsStartEnd
# bionic declares sync_file_range only for _GNU_SOURCE builds at API 26 and
# later; git falls back to fsync without it.
HAVE_SYNC_FILE_RANGE =
# bionic's libc has pthreads and clock_gettime: no libpthread or librt.
PTHREAD_LIBS =
NEEDS_LIBRT =
CURL_LDFLAGS = -lcurl -lmbedtls -lmbedx509 -lmbedcrypto
CURL_CFLAGS =
MAK
  make -j"$JOBS" SHELL=/bin/sh all
  make SHELL=/bin/sh install DESTDIR="$dest"
  # What it was built with: one program for the target, with HTTPS.
  local g=$dest$SQ_PREFIX/bin/git
  llvm-readelf -h "$g" | grep -q AArch64
  [ -x "$dest$SQ_PREFIX/libexec/git-core/git-remote-https" ]
  ! llvm-readelf -d "$g" | grep NEEDED | grep -qE 'curl|mbed|libz'
  # The phone's shell, not the build machine's: in git (run-command, aliases
  # with !) and in its scripts' first lines.
  grep -q "$SQ_PREFIX/bin/bash" "$g"
  head -n 1 "$dest$SQ_PREFIX/libexec/git-core/git-filter-branch" | grep -q "$SQ_PREFIX/bin/bash"
}

recipe_after() {
  mkdir -p share/licenses/git
  cp "$src/COPYING" share/licenses/git/COPYING
}
