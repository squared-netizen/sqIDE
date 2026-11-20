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

recipe_build() {
  # libcurl's static archive needs mbedtls' after it; CPPFLAGS and LDFLAGS
  # carry the three packages' headers and libraries (build.sh's depends).
  cat > config.mak <<MAK
uname_S = Linux
uname_M = aarch64
uname_O = Android
CC = $CC
AR = $AR
CFLAGS = $CFLAGS $CPPFLAGS
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
CURL_LDFLAGS = -lcurl -lmbedtls -lmbedx509 -lmbedcrypto
CURL_CFLAGS =
MAK
  make -j"$JOBS" all
  make install DESTDIR="$dest"
  # What it was built with: one program for the target, with HTTPS.
  local g=$dest$SQ_PREFIX/bin/git
  llvm-readelf -h "$g" | grep -q AArch64
  [ -x "$dest$SQ_PREFIX/libexec/git-core/git-remote-https" ]
  ! llvm-readelf -d "$g" | grep NEEDED | grep -qE 'curl|mbed|libz'
}

recipe_after() {
  mkdir -p share/licenses/git
  cp "$src/COPYING" share/licenses/git/COPYING
}
