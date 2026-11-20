# curl 8.23.0: transfers URLs, MIT-style (the curl licence). pkg downloads
# packages with it (pkg update, upgrade, install NAME), and it is a curl
# like any other for the shell. HTTPS through mbedtls (linked in, static);
# HTTP/1.1 only: no nghttp2, zlib, brotli, zstd, IDN or PSL, and none of
# curl's mail, LDAP, SMB, telnet or other protocols.
#
# Certificates: Android's own trust store, read as a folder of PEM files
# (--with-ca-path). /system/etc/security/cacerts is on every Android;
# Android 14 and later keep an updatable one in
# /apex/com.android.conscrypt/cacerts, and sqterm's shell points
# SSL_CERT_DIR there when it exists (curl reads SSL_CERT_DIR). mbedtls
# reads those files as they are: a certificate in PEM, with OpenSSL's text
# description of it before or after (checked 2026-11-18, both orders).
#
# In the APK: small (about 1.5 MB installed), and what pkg needs to get
# everything else.

version=8.23.0
license=curl
file=curl-$version.tar.xz
url=https://curl.se/download/$file
mirrors="https://github.com/curl/curl/releases/download/curl-${version//./_}/$file"
sha256=8dadd7d2536a314f982ad05a651b713f426bf7d02cc2c76cffb9796f2218e6c5
top=curl-$version
depends=mbedtls
keep="bin/curl share/licenses/curl"

recipe_build() {
  # mbedtls' headers and static libraries come in through CPPFLAGS and
  # LDFLAGS (build.sh's depends).
  ./configure ${SQ_HOST_TRIPLE:+--host=$SQ_HOST_TRIPLE --build=$(sh ./config.guess)} \
    --prefix="$SQ_PREFIX" --disable-shared --enable-static \
    --with-mbedtls --with-ca-path=/system/etc/security/cacerts --without-ca-bundle \
    --without-ca-fallback --without-libpsl --without-zlib --without-brotli --without-zstd \
    --without-nghttp2 --without-libidn2 --without-librtmp \
    --disable-ldap --disable-ldaps --disable-rtsp --disable-dict --disable-telnet --disable-tftp \
    --disable-pop3 --disable-imap --disable-smtp --disable-gopher --disable-mqtt --disable-smb \
    --disable-docs --disable-manual
  make -j"$JOBS"
  make install DESTDIR="$dest"
  # What it was built with: HTTPS through mbedtls, Android's trust store.
  "$SQEDIT" match -q -x -F '#define USE_MBEDTLS 1' lib/curl_config.h
  "$SQEDIT" match -q -x -F '#define CURL_CA_PATH "/system/etc/security/cacerts"' lib/curl_config.h
}

recipe_after() {
  mkdir -p share/licenses/curl
  cp "$src/COPYING" share/licenses/curl/COPYING
}
