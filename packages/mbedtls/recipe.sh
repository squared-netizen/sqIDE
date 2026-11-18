# mbedtls 4.3.0: TLS and X.509 (Arm's Mbed TLS, with its TF-PSA-Crypto
# inside), Apache-2.0. What curl links to speak HTTPS (pkg's downloads),
# static: curl carries its own copy, and this package is for building
# against it (headers and static libraries), not for running anything.
#
# Chosen over OpenSSL for size (about 2 MB of static libraries, a few
# hundred KB linked into curl) and for a CMake build with nothing to
# generate (release archives carry their generated files). The 4.x line,
# not the 3.6 LTS, which reaches its end in 2027; curl supports both.
#
# The release archive, not GitHub's tag archive: TF-PSA-Crypto and the
# framework are submodules, only the release carries them. bzip2 to unpack
# (Termux: pkg install bzip2).
#
# Not in the APK (bundled=no): nothing at run time needs it.

version=4.3.0
license="Apache-2.0 OR GPL-2.0-or-later"
file=mbedtls-$version.tar.bz2
url=https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$version/$file
mirrors=
sha256=1c87cba094cb40f097ebe0eba440a74939b71dab4e1d0d566bc90749ff381d18
top=mbedtls-$version
depends=
bundled=no
keep="include lib/libmbedtls.a lib/libmbedx509.a lib/libmbedcrypto.a lib/libtfpsacrypto.a
      share/licenses/mbedtls"

recipe_build() {
  local -a android=()
  if [ -n "${SQ_NDK:-}" ]; then
    android=(-DCMAKE_TOOLCHAIN_FILE="$SQ_NDK/build/cmake/android.toolchain.cmake"
             -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM="android-$SQ_API")
  else
    android=(-DCMAKE_C_COMPILER="${CC%% *}")
  fi
  cmake -S . -B ../build -G Ninja "${android[@]}" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$SQ_PREFIX" -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF -DGEN_FILES=OFF \
    -DUSE_SHARED_MBEDTLS_LIBRARY=OFF -DUSE_STATIC_MBEDTLS_LIBRARY=ON
  ninja -C ../build -j"$JOBS"
  DESTDIR="$dest" ninja -C ../build install
}

recipe_after() {
  mkdir -p share/licenses/mbedtls
  cp "$src/LICENSE" share/licenses/mbedtls/LICENSE
}
