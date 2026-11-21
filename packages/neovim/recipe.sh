# neovim: the editor (Apache-2.0, and Vim's licence for the parts from Vim).
# Built as its own release builds it: neovim's cmake.deps makes the libraries
# it needs (libuv, luv, lpeg, unibilium, utf8proc, tree-sitter and its
# parsers), statically, then neovim links them. Their sources are this
# recipe's `extra` files, at the versions and sha256s neovim 0.12.6's
# cmake.deps/deps.txt names, put where cmake.deps looks before it downloads
# (build/downloads/<name>/): the build makes no network request.
#
# LuaJIT is built here, not by cmake.deps: its build compiles a helper
# (buildvm) to run during the build, which in a cross build must be for the
# build machine (HOST_CC) while LuaJIT itself is for the phone.
#
# A cross build (CI) runs neovim's code generators, which are Lua run by
# LuaJIT with neovim's nlua0 module, so it builds neovim for the build
# machine first and gives the phone's build that machine's luajit, nlua0
# and nvim (NLUA0_HOST_PRG, NVIM_HOST_PRG), as Termux does. On the phone
# the build runs its own.
#
# Needs CMake on the build machine (CI has it; sqterm has no cmake package
# yet, so this recipe builds on CI, not on the phone).

version=0.12.6
license="Apache-2.0 and Vim"
file=neovim-$version.tar.gz
url=https://github.com/neovim/neovim/archive/refs/tags/v$version.tar.gz
sha256=8629fdc5eb951daf89a76f68cad12f2f1e03fab0d539150ba3fe6d177d254b7f
top=neovim-$version
depends=
bundled=no
keep="bin/nvim lib/nvim share/nvim share/licenses/neovim"
# "SUB URL SHA256": neovim 0.12.6's cmake.deps/deps.txt. SUB is the name
# cmake.deps downloads it as (its DOWNLOAD_DIR), but luajit, which is built
# here.
extra="
luajit https://github.com/luajit/luajit/archive/fbb36bb6bfa88716a47c58bcf9ce9f2ef752abac.tar.gz e60cd2f3057aa39d33d1c895a20087ab54494d9a1ca45aa488ff0378af42df48
libuv https://github.com/libuv/libuv/archive/v1.52.1.tar.gz 478baf2599bfbc882c355288c9cb6f92e0e7dda435fa04031fa5b607cf3f414c
lpeg https://github.com/neovim/deps/raw/d495ee6f79e7962a53ad79670cb92488abe0b9b4/opt/lpeg-1.1.0.tar.gz 4b155d67d2246c1ffa7ad7bc466c1ea899bbc40fef0257cc9c03cecbaed4352a
luv https://github.com/luvit/luv/archive/1.52.1-0.tar.gz e8b8774b31d24be4fcf2b021b90599ecccc8e476c61efcc59c3c10cab813a885
lua_compat53 https://github.com/lunarmodules/lua-compat-5.3/archive/v0.13.tar.gz f5dc30e7b1fda856ee4d392be457642c1f0c259264a9b9bfbcb680302ce88fc2
unibilium https://github.com/neovim/unibilium/archive/v2.1.2.tar.gz 370ecb07fbbc20d91d1b350c55f1c806b06bf86797e164081ccc977fc9b3af7a
utf8proc https://github.com/juliastrings/utf8proc/archive/v2.11.3.tar.gz abfed50b6d4da51345713661370290f4f4747263ee73dc90356299dfc7990c78
treesitter https://github.com/tree-sitter/tree-sitter/archive/v0.26.13.tar.gz ece24c3c5e2a76384075e830c7139b59fce8fb01e4ef8436fab08bbe10444c89
treesitter_c https://github.com/tree-sitter/tree-sitter-c/archive/v0.24.1.tar.gz 25dd4bb3dec770769a407e0fc803f424ce02c494a56ce95fedc525316dcf9b48
treesitter_lua https://github.com/tree-sitter-grammars/tree-sitter-lua/archive/v0.5.0.tar.gz cf01b93f4b61b96a6d27942cf28eeda4cbce7d503c3bef773a8930b3d778a2d9
treesitter_vim https://github.com/tree-sitter-grammars/tree-sitter-vim/archive/v0.8.1.tar.gz 93cafb9a0269420362454ace725a118ff1c3e08dcdfdc228aa86334b54d53c2a
treesitter_vimdoc https://github.com/neovim/tree-sitter-vimdoc/archive/v4.1.0.tar.gz 020e8f117f648c8697fca967995c342e92dbd81dab137a115cc7555207fbc84f
treesitter_query https://github.com/tree-sitter-grammars/tree-sitter-query/archive/v0.8.0.tar.gz c2b23b9a54cffcc999ded4a5d3949daf338bebb7945dece229f832332e6e6a7d
treesitter_markdown https://github.com/tree-sitter-grammars/tree-sitter-markdown/archive/v0.5.3.tar.gz df845b1ab7c7c163ec57d7fa17170c92b04be199bddab02523636efec5224ab6
"

# cmake.deps' downloads, put where it looks: <deps>/build/downloads/<SUB>/.
neovim_downloads() {
  local deps=$1 sub _url _sha var
  while read -r sub _url _sha; do
    [ -n "$sub" ] && [ "$sub" != luajit ] || continue
    var=extra_$sub
    mkdir -p "$deps/build/downloads/$sub"
    cp "${!var}" "$deps/build/downloads/$sub/${_url##*/}"
  done <<< "$extra"
}

# LuaJIT into PREFIX, static: for the phone with CC, its buildvm with HOST_CC.
neovim_luajit() {
  local prefix=$1 cc=$2 host_cc=$3 dir
  dir=$(mktemp -d "$PWD/luajit.XXXXXX")
  tar -xzf "$extra_luajit" -C "$dir" --strip-components=1
  make -C "$dir" -j"$JOBS" amalg BUILDMODE=static PREFIX="$prefix" CC="$cc" HOST_CC="$host_cc" \
    TARGET_STRIP=: Q=
  make -C "$dir" install PREFIX="$prefix" LDCONFIG=: Q=
}

recipe_patch() {
  # neovim's XDG defaults name /etc/xdg and /usr/share, which Android's app
  # cannot use: the prefix's.
  "$SQEDIT" sub -F --in-place --expect 1 '"/etc/xdg/"' "\"$SQ_PREFIX/etc/xdg/\"" src/nvim/os/stdpaths.c
  "$SQEDIT" sub -F --in-place --expect 1 '"/usr/local/share/:/usr/share/"' \
    "\"$SQ_PREFIX/local/share/:$SQ_PREFIX/share/\"" src/nvim/os/stdpaths.c
  # cmake.deps gives each library's build the compiler and nothing of a
  # toolchain file: the NDK's, so each is built for Android (libuv links no
  # librt there, and the rest find Android's headers); and the libraries
  # already built as a find root, which CMake's Android setup appends the
  # sysroot to (luv found no libuv under the sysroot alone).
  # ({{ and }} are braces in sqedit's replacement.)
  "$SQEDIT" sub -F --in-place --expect 1 '  -D CMAKE_C_COMPILER=${CMAKE_C_COMPILER}' \
    '  -D CMAKE_C_COMPILER=${{CMAKE_C_COMPILER}}
  -D CMAKE_TOOLCHAIN_FILE=${{CMAKE_TOOLCHAIN_FILE}}
  -D ANDROID_ABI=${{ANDROID_ABI}}
  -D ANDROID_PLATFORM=${{ANDROID_PLATFORM}}
  -D CMAKE_FIND_ROOT_PATH=${{DEPS_INSTALL_DIR}}' cmake/Deps.cmake
}

recipe_build() {
  local here=$PWD host=$PWD/host
  local ndk_cmake=$SQ_NDK/build/cmake/android.toolchain.cmake
  [ -f "$ndk_cmake" ] || { echo "neovim: no $ndk_cmake" >&2; return 1; }
  command -v cmake >/dev/null || { echo "neovim: needs cmake on the build machine" >&2; return 1; }
  local lua_gen nlua0_host="" nvim_host=""
  # LuaJIT as built here, named to every CMake step (cmake.deps looks for a
  # system one when it does not build its own).
  luajit_at() { echo "-DLUAJIT_INCLUDE_DIR=$1/include/luajit-2.1" "-DLUAJIT_LIBRARY=$1/lib/libluajit-5.1.a"; }

  if [ "$SQ_CROSS" = 1 ]; then
    # neovim for the build machine, for its code generators.
    neovim_luajit "$host/deps/usr" "$CC_FOR_BUILD" "$CC_FOR_BUILD"
    neovim_downloads "$host/deps"
    ( unset CC CFLAGS LDFLAGS CPPFLAGS AR RANLIB STRIP
      cmake -S cmake.deps -B "$host/deps" -DCMAKE_BUILD_TYPE=Release -DUSE_BUNDLED_LUAJIT=OFF \
        -DUSE_BUNDLED_GETTEXT=OFF -DUSE_BUNDLED_LIBICONV=OFF $(luajit_at "$host/deps/usr")
      cmake --build "$host/deps" -j"$JOBS"
      cmake -S . -B "$host/build" -DCMAKE_BUILD_TYPE=Release -DDEPS_PREFIX="$host/deps/usr" \
        -DENABLE_LIBINTL=OFF -DCMAKE_INSTALL_PREFIX="$host/usr" $(luajit_at "$host/deps/usr")
      cmake --build "$host/build" -j"$JOBS" --target nvim_bin nlua0 )
    lua_gen=$host/deps/usr/bin/luajit
    nlua0_host=$(find "$host/build" -name 'libnlua0.so' -o -name 'nlua0.so' | head -n 1)
    nvim_host=$host/build/bin/nvim
    [ -x "$nvim_host" ] && [ -n "$nlua0_host" ] || { echo "neovim: the build machine's nvim was not built" >&2; return 1; }
  fi

  # For the phone: LuaJIT, then the rest of the libraries, then neovim.
  local deps=$here/deps
  local -a android=(-DCMAKE_TOOLCHAIN_FILE="$ndk_cmake" -DANDROID_ABI=arm64-v8a
                    -DANDROID_PLATFORM="$SQ_API" -DCMAKE_BUILD_TYPE=Release)
  neovim_luajit "$deps/usr" "$CC" "$CC_FOR_BUILD"
  [ "$SQ_CROSS" = 1 ] || lua_gen=$deps/usr/bin/luajit
  neovim_downloads "$deps"
  # unibilium reads the terminfo sqterm's ncurses package installs.
  ( export CFLAGS="-DTERMINFO_DIRS=\\\"$SQ_PREFIX/share/terminfo\\\""
    cmake -S cmake.deps -B "$deps" "${android[@]}" -DUSE_BUNDLED_LUAJIT=OFF \
      -DUSE_BUNDLED_GETTEXT=OFF -DUSE_BUNDLED_LIBICONV=OFF $(luajit_at "$deps/usr")
    cmake --build "$deps" -j"$JOBS" )
  local -a host_prgs=()
  [ "$SQ_CROSS" = 1 ] && host_prgs=(-DNLUA0_HOST_PRG="$nlua0_host" -DNVIM_HOST_PRG="$nvim_host")
  cmake -S . -B build "${android[@]}" -DDEPS_PREFIX="$deps/usr" -DCMAKE_INSTALL_PREFIX="$SQ_PREFIX" \
    -DCMAKE_FIND_ROOT_PATH="$deps/usr" -DENABLE_LIBINTL=OFF -DCOMPILE_LUA=OFF \
    -DLUA_GEN_PRG="$lua_gen" "${host_prgs[@]}" $(luajit_at "$deps/usr")
  cmake --build build -j"$JOBS"
  DESTDIR="$dest" cmake --install build

  # What it was built with, each check saying so when it fails.
  local nvim=$dest$SQ_PREFIX/bin/nvim
  llvm-readelf -h "$nvim" | "$SQEDIT" match -q -F AArch64 || { echo "neovim: not an arm64 program" >&2; return 1; }
  llvm-readelf -l "$nvim" | "$SQEDIT" match -q -F /system/bin/linker64 || { echo "neovim: not for Android" >&2; return 1; }
  [ -f "$dest$SQ_PREFIX/share/nvim/runtime/syntax/syntax.vim" ] || { echo "neovim: no runtime files" >&2; return 1; }
  ls "$dest$SQ_PREFIX/lib/nvim/parser" >&2 || { echo "neovim: no tree-sitter parsers" >&2; return 1; }
  llvm-readelf -d "$nvim" | "$SQEDIT" match -F NEEDED
}

recipe_after() {
  mkdir -p share/licenses/neovim
  cp "$src/LICENSE.txt" share/licenses/neovim/LICENSE.txt
}
