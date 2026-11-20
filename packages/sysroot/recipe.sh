# sysroot: what clang compiles and links against for sqterm's prefix, made
# from AOSP's bionic (Apache-2.0 and BSD), not from the NDK (step 3).
#
# share/sysroot/usr/include               bionic's headers and the kernel's
#                                         (uapi, as bionic carries them)
# share/sysroot/usr/include/aarch64-linux-android/asm   the arm64 asm headers
# share/sysroot/usr/lib/aarch64-linux-android/28/       libc.so, libm.so and
#                                         libdl.so as link-time stubs (empty
#                                         definitions with the right symbol
#                                         versions: the real ones are the
#                                         device's), and the crt objects
# bin/clang.cfg, bin/clang++.cfg          clang's default configuration:
#                                         --sysroot=<the above, absolute>, so plain
#                                         `clang hello.c` finds it
#
# The stubs come from bionic's symbol lists (libc.map.txt and the others)
# through stubgen.awk: what an app may call at API 28, nothing newer, nothing
# for the platform alone. The NDK does the same with AOSP's ndkstubgen.
#
# Built for one API level, 28, on CI; making the stubs for another level
# when pkg installs it is a later step (stubgen.awk runs anywhere awk does).
#
# Android's own API, as the NDK has it ("One C++23 toolchain", phase 2):
# its headers (android/*.h, jni.h, EGL, GLES) and stubs for the libraries an
# app links beyond bionic: liblog, libandroid, libnativewindow, libEGL and
# libGLESv1_CM/v2/v3. From four more of AOSP's repositories at the same tag
# (`also`), a few folders of each. With them, an Android app builds against
# this sysroot as against the NDK's.
#
# Not yet: bits/glibc-syscalls.h (sys/syscall.h; AOSP generates it), and the
# NDK's rarer libraries (camera, media, aaudio, vulkan, jnigraphics).

version=16.0.0_r1
license="Apache-2.0 and BSD (AOSP bionic)"
git=https://android.googlesource.com/platform/bionic
tag=android-$version
# More of AOSP at the same tag: "SUB URL FOLDER...", read as $src_SUB.
also="
native  https://android.googlesource.com/platform/frameworks/native  include/android libs/arect libs/nativewindow opengl/include opengl/libs
logging https://android.googlesource.com/platform/system/logging     liblog/include
jni     https://android.googlesource.com/platform/libnativehelper     include_jni
base    https://android.googlesource.com/platform/frameworks/base     native/android
"
file=
top=
depends=
bundled=no
inputs="packages/sysroot/stubgen.awk"
keep="share/sysroot bin/clang.cfg bin/clang++.cfg share/licenses/sysroot"

recipe_build() {
  local root=$dest$SQ_PREFIX/share/sysroot
  local inc=$root/usr/include lib=$root/usr/lib/aarch64-linux-android/$SQ_API
  local clang=${CC%% *}
  local k=$src/libc/kernel
  mkdir -p "$inc/aarch64-linux-android" "$lib"
  # Headers: bionic's, then the kernel's uapi (all but the per-arch asm
  # folders), bionic's Android additions to it, and arm64's asm.
  cp -R "$src/libc/include/." "$inc/"
  for d in "$k/uapi"/*; do
    # Not the per-arch asm folders (arm64's goes below); asm-generic, yes.
    case $(basename "$d") in asm-arm | asm-arm64 | asm-riscv | asm-x86) ;; *) cp -R "$d" "$inc/" ;; esac
  done
  cp -R "$k/android/uapi/." "$inc/"
  cp -R "$k/uapi/asm-arm64/asm" "$inc/aarch64-linux-android/asm"

  # A library's stub: empty definitions with its symbol versions, from its
  # symbol list (stubgen.awk).
  stub() {
    local l=$1 map=$2
    awk -v api="$SQ_API" -v arch=arm64 -v mode=c -f "$ROOT/packages/sysroot/stubgen.awk" "$map" > "$l.c"
    awk -v api="$SQ_API" -v arch=arm64 -v mode=map -f "$ROOT/packages/sysroot/stubgen.awk" "$map" > "$l.map"
    "$clang" --target="aarch64-linux-android$SQ_API" -nostdlib -shared -fPIC -fno-builtin -w \
      -fuse-ld=lld -Wl,--version-script="$l.map" -Wl,-soname,"$l.so" \
      -Wl,-z,max-page-size=16384 "$l.c" -o "$lib/$l.so"
    echo "$l.so: $(grep -c '(void) {}\|= 0;' "$l.c") symbols"
  }

  # Stubs: libc, libm, libdl, from their symbol lists.
  local l map
  for l in libc libm libdl; do
    # libc/libc.map.txt, libm/libm.map.txt, libdl/libdl.map.txt.
    map=
    for candidate in "$src/$l/$l.map.txt" "$src/libc/$l.map.txt"; do
      if [ -f "$candidate" ]; then map=$candidate; break; fi
    done
    [ -n "$map" ] || { echo "no symbol list for $l" >&2; return 1; }
    stub "$l" "$map"
  done

  # Android's API beyond bionic: its headers (the links among them
  # followed), then its libraries' stubs, from their symbol lists.
  # A folder's files into another, links followed; a link to a file
  # outside the folders checked out is left out, and said.
  copy_files() {
    local from=$1 to=$2 f
    (cd "$from" && find . -type l ! -exec test -e {} \; -print) | sed 's/^/left out (a link to nothing): /'
    (cd "$from" && find -L . -type f) | while read -r f; do
      mkdir -p "$to/$(dirname "$f")"
      cp -L "$from/$f" "$to/$f"
    done
  }
  local d
  for d in "$src_native/include/android" "$src_native/libs/arect/include/android" \
           "$src_native/libs/nativewindow/include/android" "$src_logging/liblog/include/android"; do
    copy_files "$d" "$inc/android"
  done
  copy_files "$src_native/opengl/include" "$inc"
  cp -L "$src_jni/include_jni/jni.h" "$inc/jni.h"
  local -a android_maps=(
    "liblog $src_logging/liblog/liblog.map.txt"
    "libandroid $src_base/native/android/libandroid.map.txt"
    "libnativewindow $src_native/libs/nativewindow/libnativewindow.map.txt"
    "libEGL $src_native/opengl/libs/libEGL.map.txt"
    "libGLESv1_CM $src_native/opengl/libs/libGLESv1_CM.map.txt"
    "libGLESv2 $src_native/opengl/libs/libGLESv2.map.txt"
    "libGLESv3 $src_native/opengl/libs/libGLESv3.map.txt"
  )
  local entry
  for entry in "${android_maps[@]}"; do
    l=${entry%% *} map=${entry#* }
    [ -f "$map" ] || { echo "no symbol list for $l: $map" >&2; return 1; }
    stub "$l" "$map"
  done

  # The crt objects, as bionic builds them (libc/Android.bp), against the
  # headers just installed.
  local b=$src/libc/arch-common/bionic
  local -a cflags=(--target="aarch64-linux-android$SQ_API" --sysroot="$root" -O2 -fPIC
                   -I"$src/libc" -I"$b" -DPLATFORM_SDK_VERSION="$SQ_API")
  "$clang" "${cflags[@]}" -c "$b/crtbrand.S" -o crtbrand.o
  "$clang" "${cflags[@]}" -c "$b/crtbegin.c" -o crtbegin.o
  "$clang" "${cflags[@]}" -c "$b/crtbegin_so.c" -o crtbegin_so_c.o
  ld.lld -r crtbegin.o crtbrand.o -o "$lib/crtbegin_dynamic.o"
  cp "$lib/crtbegin_dynamic.o" "$lib/crtbegin_static.o"
  ld.lld -r crtbegin_so_c.o crtbrand.o -o "$lib/crtbegin_so.o"
  "$clang" "${cflags[@]}" -c "$b/crtend.S" -o "$lib/crtend_android.o"
  "$clang" "${cflags[@]}" -c "$b/crtend_so.S" -o "$lib/crtend_so.o"

  # clang's default configuration (clang 16 and later read <driver>.cfg
  # beside the binary).
  mkdir -p "$dest$SQ_PREFIX/bin"
  # -D__BIONIC_CTYPE_INLINE=inline: bionic's <ctype.h> makes isalpha and
  # the rest `static inline` otherwise, which `import std` cannot export
  # (libc++'s std module re-exports them); inline, they are the ones libc
  # exports. squared's own Android build passes the same.
  # The sysroot by its absolute path: every package is built for this one
  # prefix. Not <CFG> (the config file's folder): clang 23.1.5 on the phone
  # passed it on unexpanded, in either spelling (2026-11-17: stdio.h not
  # found, `-isysroot <CFG>/../share/sysroot`).
  printf '%s\n' "--sysroot=$SQ_PREFIX/share/sysroot" '-D__BIONIC_CTYPE_INLINE=inline' \
    > "$dest$SQ_PREFIX/bin/clang.cfg"
  cp "$dest$SQ_PREFIX/bin/clang.cfg" "$dest$SQ_PREFIX/bin/clang++.cfg"

  # The proof: a C program compiles and links against it (the build
  # machine's clang, its own runtime), and needs only the device's libc.
  printf '#include <stdio.h>\nint main(void) { puts("hello from the sysroot"); return 0; }\n' > hello.c
  "$clang" --target="aarch64-linux-android$SQ_API" --sysroot="$root" -fuse-ld=lld hello.c -o hello
  llvm-readelf -d hello | grep -q 'Shared library: \[libc.so\]'
  echo "hello: links against the sysroot"

  # And an Android app's: every header squared's app includes, and every
  # library it links (no -l is the NDK's alone).
  cat > app.c <<'C'
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <GLES3/gl3.h>
#include <android/asset_manager.h>
#include <android/configuration.h>
#include <android/keycodes.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <jni.h>
void ANativeActivity_onCreate(ANativeActivity* a, void* state, size_t size) {
  (void)state; (void)size;
  __android_log_print(ANDROID_LOG_INFO, "sysroot", "%d", AConfiguration_getSdkVersion(0));
  ANativeWindow_getWidth(0); AAssetManager_open(a->assetManager, "x", AASSET_MODE_BUFFER);
  ALooper_forThread(); eglGetDisplay(EGL_DEFAULT_DISPLAY); glClear(GL_COLOR_BUFFER_BIT);
}
C
  "$clang" --target="aarch64-linux-android$SQ_API" --sysroot="$root" -fuse-ld=lld -shared -fPIC \
    app.c -o libapp.so -landroid -llog -lEGL -lGLESv3
  llvm-readelf -d libapp.so | grep -q 'Shared library: \[libandroid.so\]'
  echo "libapp.so: an Android app links against the sysroot"
}

recipe_after() {
  mkdir -p share/licenses/sysroot
  cp "$src/libc/NOTICE" share/licenses/sysroot/NOTICE
  # The other repositories' licences: Apache-2.0 (frameworks, liblog,
  # libnativehelper); Khronos' headers carry their own (MIT-style) notices.
  local sub f
  for sub in native logging jni base; do
    f=src_$sub
    f=${!f}
    for n in NOTICE LICENSE; do
      [ -f "$f/$n" ] && cp "$f/$n" "share/licenses/sysroot/$n.$sub"
    done
  done
  return 0
}
