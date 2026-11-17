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
# Not yet: bits/glibc-syscalls.h (sys/syscall.h; AOSP generates it), and
# the libraries an Android app links beyond bionic (liblog, libandroid).

version=16.0.0_r1
license="Apache-2.0 and BSD (AOSP bionic)"
git=https://android.googlesource.com/platform/bionic
tag=android-$version
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

  # Stubs: libc, libm, libdl, from their symbol lists.
  local l map
  for l in libc libm libdl; do
    # libc/libc.map.txt, libm/libm.map.txt, libdl/libdl.map.txt.
    map=
    for candidate in "$src/$l/$l.map.txt" "$src/libc/$l.map.txt"; do
      if [ -f "$candidate" ]; then map=$candidate; break; fi
    done
    [ -n "$map" ] || { echo "no symbol list for $l" >&2; return 1; }
    awk -v api="$SQ_API" -v arch=arm64 -v mode=c -f "$ROOT/packages/sysroot/stubgen.awk" "$map" > "$l.c"
    awk -v api="$SQ_API" -v arch=arm64 -v mode=map -f "$ROOT/packages/sysroot/stubgen.awk" "$map" > "$l.map"
    "$clang" --target="aarch64-linux-android$SQ_API" -nostdlib -shared -fPIC -fno-builtin -w \
      -fuse-ld=lld -Wl,--version-script="$l.map" -Wl,-soname,"$l.so" \
      -Wl,-z,max-page-size=16384 "$l.c" -o "$lib/$l.so"
    echo "$l.so: $(grep -c '(void) {}\|= 0;' "$l.c") symbols"
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
}

recipe_after() {
  mkdir -p share/licenses/sysroot
  cp "$src/libc/NOTICE" share/licenses/sysroot/NOTICE
}
