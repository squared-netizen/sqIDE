# packages: sqterm's prefix

sqterm runs its programs from its own folder, `/data/data/dev.squared.sqterm/files/usr`
(`$PREFIX` in its shell), as Termux runs its own. What is in it comes from
packages built here, carried in the APK (`sq_android/assets/packages/`), and
unpacked by the app the first time it starts. Decided in Sprint 12 (the plan's
12.0 and 12.5): squared's own recipes, each tool's latest stable release, only
the tools in use, C, C++ or Lua only. GitHub Actions builds them with the same
script (`.github/workflows/packages.yml`, decided 2026-11-04) and publishes
them as a release.

**Offline.** No build makes a network request: `make packages` and
`packages/build.sh` work from what is already on disk. The one step that
downloads is `tools/fetch_packages.sh`, run on purpose: CI's packages, or the
sources to build them here.

```text
build.sh          checks the sources, builds and packs every recipe, writes the
                  index; offline
../tools/fetch_packages.sh  the download step: CI's packages, or --sources
sources.lock      sha256s of the sources that publish none, from their first download
ncurses/recipe.sh ncurses 6.6: the terminal database (sqterm's entry in it), tic, tput
zsh/recipe.sh     zsh 5.9.2: the shell, static, its modules linked in
zsh/zshrc         $PREFIX/etc/zshrc: keys, history, completion, a prompt; then
                  what fish does out of the box (colours for files, completion
                  that ignores case and takes part of a name, Up and Down
                  searching the history by what is typed, a prompt with the
                  failed status and the git branch) and the two plugins below
                  when installed. SQTERM_PLAIN=1 in ~/.zshenv: none of that
zsh-autosuggestions/, zsh-syntax-highlighting/
                  zsh scripts (0.7.1, MIT; 0.8.0, BSD), git sources: the rest
                  of a line offered in grey as it is typed, and the line
                  coloured (a command that does not exist in red)
pkg/recipe.sh     pkg (sq_app/pkg/pkg.cpp): list, files, update, upgrade,
                  search, install (from the release, or a file), remove,
                  backup; sqterm's own, built from this repository (no
                  download)
mbedtls/, curl/   Mbed TLS 4.3.0 (Apache-2.0; static, for building) and curl
                  8.23.0 (curl licence; HTTPS, in the APK): pkg's downloads
bash/ coreutils/ sed/ grep/ gawk/ make/
                  GNU (GPLv3): bash 5.3, coreutils 9.8 (one binary, symbolic
                  links), sed 4.9, grep 3.12, gawk 5.3.2
                  (awk), make 4.4.1 (recipes run under /system/bin/sh). No findutils:
                  its gnulib wants mktime_z (bionic: API 35); Android's own
                  find and xargs (toybox, /system/bin) serve
```

## Getting them

On the phone, in Termux, from sqterm's folder. Either CI's packages:

```sh
pkg install curl                  # once
tools/fetch_packages.sh           # the network step: CI's release for these recipes
make dev                          # offline: the APK, with them
```

or the sources, built here (a recipe changed here has no release until CI
builds it from `main`):

```sh
pkg install curl xz-utils zstd    # once
tools/fetch_packages.sh --sources # the network step: the recipes' sources
make packages                     # offline; or: bash packages/build.sh [name...]
make dev
```

**One key a package.** `bash packages/build.sh --keys` prints each package's
name, version and key: 16 hex digits of a sha256 over its recipe folder (not
the READMEs), the files outside it the recipe names in `inputs` (ncurses:
`terminfo/sqterm.ti`), `build.sh`, its line in `sources.lock`, the API level,
and the keys of what it depends on. A recipe changed changes that package's
key and the keys of what depends on it, nothing else.

`tools/fetch_packages.sh` downloads `<name>-<version>-<key>.tar.zst` for each
recipe as it is in this folder, from the GitHub release named `packages`,
checks each against the `.sha256` beside it, and only once all have arrived
and matched replaces what was in `sq_android/assets/packages/` (not kept in
git; the APK build packs it) and writes the index. The repository is public:
no token. Those sha256s guard against a broken download; they come from the
same release, so they are not a signature. `--dry-run` says what it would
download.

CI builds only the packages whose archive the release lacks, when a change
to `packages/` or `terminfo/sqterm.ti` reaches `main`, or when started by
hand, and adds them to the release. Finished builds are kept in the Actions
cache, and `build.sh` skips a package whose key, archive and install are
already there (here as on CI), so an up-to-date package is not built again
for one that depends on it. A pull request builds too, and keeps the result
as a workflow artifact without publishing it. CI runs the same two steps:
the sources first, then the offline build.

The first build here takes several minutes (ncurses, then zsh). Commit
`packages/sources.lock` when it changes: ncurses publishes a signature but no
sha256, so `fetch_packages.sh --sources` records its first download there and
the build checks every later one against it. CI never records one
(`SQ_LOCKED=1`): a new source without a published sha256 is fetched on the
phone once first.

`bash packages/build.sh --host-test` builds the same recipes for the machine
it runs on, into `build/packages/host/` (nothing goes into the APK): a check of
a recipe where there is no NDK. `SQ_HOST_PREFIX=<dir>` builds them for where
they will be unpacked, if that is elsewhere (sqterm's host test harness).

**A cross build.** On CI's x86 Linux nothing built for Android can run.
`build.sh` says so to the recipes (`SQ_CROSS=1`; 0 on an arm64 Android), and
they give configure the answers it cannot find (zsh's cache values) and
build what they must run for the build machine with `CC_FOR_BUILD` (ncurses:
its build helpers, and a second `tic` that compiles the terminfo entries).

**No hard links.** Android refuses them in an app's files, Termux's home
included (the first `make packages` on the phone, 2026-11-04: ncurses' tic,
"cannot link l/lpr to p/printer"). A recipe makes symbolic links instead:
ncurses is configured `--enable-symlinks`, zsh installs with `LN=cp`. A
host test can be made to refuse them the same way, with a preloaded
`link`/`linkat` that fail.

## Large packages (bundled=no)

A recipe with `bundled=no` (llvm: clang and lld, LLVM 23.1.5) is built and
published like the others but not carried in the APK: its archive goes to
`build/packages/extra/`, the index leaves it out, and
`tools/fetch_packages.sh` skips it. Get it on purpose and install it with
pkg, so it is never stored twice:

```sh
pkg install llvm                            # in sqterm: from the release (below)
```

or, without the network in sqterm, through Termux and the packages folder:

```sh
tools/fetch_packages.sh --get llvm          # Termux: llvm, and what it needs, into /sdcard/sqterm/packages
pkg install llvm                            # in sqterm: found there by name, offline
```

llvm is built on CI only (hours on a phone): CMake and Ninja, the NDK's CMake
toolchain, LLVM's tablegen built for the build machine first
(`CXX_FOR_BUILD`), ccache kept between runs.

**sysroot** (`bundled=no`) is what that clang compiles and links against:
bionic's headers and the kernel's, libc, libm and libdl as link-time stubs
for API 28, and the crt objects, all from AOSP's bionic (a git source,
`git=` and `tag=` in the recipe, its commit in `sources.lock`), not from the
NDK. `stubgen.awk` turns bionic's symbol lists into the stubs. It also
installs `bin/clang.cfg` and `bin/clang++.cfg`, so plain `clang` uses it.

**runtimes** (`bundled=no`): compiler-rt's builtins, libunwind, libc++abi and
libc++ 23, static, with libc++'s module sources (`import std`), built from
the same LLVM sources against the sysroot by a clang 23 for the build
machine (apt.llvm.org's on CI). With llvm and sysroot, clang in sqterm
compiles, links and runs C and C++ programs, modules included. Also one
shared library, `libc++_shared.so` (linked in `lib/` too), for a program and
the shared libraries it loads: plain `clang++` still links libc++ in
statically, and `-nostdlib++ -lc++_shared` asks for the shared one.

**llvm-tools** (`bundled=no`): clangd, clang-tidy and clang-doc, static
binaries, built as llvm is.

**lldb** (`bundled=no`): lldb, lldb-server, lldb-dap (for an editor) and
lldb-argdumper, front ends to `liblldb.so`; all of them use the runtimes'
`libc++_shared.so`, found through a RUNPATH of `$PREFIX/lib`, so install runtimes
first. Built as runtimes are (clang 23 for the build machine, the sysroot,
no NDK), with CMake told the system is Android. No Python or Lua scripting,
line editing or curses GUI yet: each is a library to build first.

**mbedtls** (`bundled=no`): Mbed TLS 4.3.0, TLS and X.509, static libraries
and headers, built with CMake. curl links it in; nothing runs it.

**curl** (in the APK): curl 8.23.0, HTTP/1.1 and HTTPS through mbedtls, with
Android's own trust store (`/system/etc/security/cacerts`, or the updatable
one in `/apex/com.android.conscrypt/cacerts`, which the shell names in
`SSL_CERT_DIR`). What pkg downloads with.

## The manifest, and pkg

Every package carries `SQ-INF/manifest.json`, written by `build.sh`: the
squared-cartridge envelope (sqcart's spec, kind `package`: `id`, `version`,
`license`), and under `consumers.sqterm` the package's `key`, `api`,
`built_with` and `files` (every file and link it installs, not
directories). The app's first start and `pkg install` unpack it to
`$PREFIX/var/lib/sqterm/pkg/<name>/manifest.json`, so packages never write
over each other's.

`pkg` reads those records:

```sh
pkg list                          # installed: name, version, key (and the index's, if it differs)
pkg files zsh                     # what zsh installed
pkg update                        # settle the index: what there is, what differs here
pkg upgrade                       # every installed package whose key differs, and missing needs
pkg upgrade runtimes lldb         # or only those
pkg search llvm                   # what there is, what it needs, which are installed
pkg install lldb                  # by name: runtimes first (lldb needs it), then lldb
pkg install ./zsh-5.9.2.tar       # or a file (what it needs, too, if an index knows it)
pkg remove runtimes               # refused while lldb is installed (--force to insist)
pkg backup zsh /sdcard/Download   # zsh-5.9.2.tar: its files as a package again
pkg clean                         # empty the internal archive
```

**Names, not file names.** Every package is installed, upgraded and removed
by its name. The keys and sha256s in archive names are pkg's to match, never
anyone's to type.

**The index.** CI publishes, beside the archives, an `index`: `sqterm
release index 2`, then a line a package, `name version key bundled sha256
needs`, for what main's recipes ask for, in build order (rewritten on every
run on main). `needs` is what the package needs at run time, from
[`needs`](needs) (comma-separated, or `-`). `tools/fetch_packages.sh --get`
writes an index of the same form into the packages folder for what it
fetched. `pkg update` (and upgrade, install NAME and search, which start
with it) settles one: the release's, through curl, with what only the
packages folder's index names after it; offline, the packages folder's (or
the last one settled on, whichever is newer). It keeps it in
`$PREFIX/var/lib/sqterm/release-index`. pkg also reads index version 1
(no needs).

**Where the archives are.** For each package it installs, pkg looks for
that very archive (by its name, checked against the index's sha256) in:

| Folder | What it is |
|---|---|
| `$PREFIX/var/cache/sqterm/archive` | the internal archive: what pkg downloaded, kept, one archive a package (`pkg clean` empties it) |
| `/sdcard/sqterm/packages` (`$PKG_DIR`) | the packages folder: Termux writes it (`fetch_packages.sh --get`), and it survives the app |
| `/sdcard/Download` | a browser's |

and only then downloads it from the release, into the internal archive. So
`update` and `upgrade` work offline from the packages folder, a package
reinstalled needs no network, and nothing is downloaded twice. A name no
index knows is installed from the newest archive of it in the packages
folder, then Download, unchecked.

A package is compared by key, not by version: two builds of `runtimes
23.1.5` are two keys (2026-11-18: an old runtimes without
`libc++_shared.so` read as up to date). `SQ_PACKAGES_URL` points pkg at a
mirror (`<url>/packages/<file>`), as it does fetch_packages.sh.

The sha256s guard against a broken or mixed-up file, and HTTPS the way
there; they are not signatures. Packages the APK carries and pkg has
upgraded are put back to the APK's own when an update of the app brings a
different set.

An upgrade (`pkg install` of a newer version) deletes the files the old
version had and the new one has not, and the folders left empty. `--consume`
deletes the file once it is installed, so a large package is not kept twice.
`backup` writes a plain tar (`pkg install` takes it as it takes a
.tar.zst). Reading shared storage needs the Files permission, granted once:
Settings, Apps, sqterm, Permissions, Files.

## What a recipe is

`packages/<name>/recipe.sh`, sourced by `build.sh`:

| Variable or function | What |
|---|---|
| `version`, `file`, `url`, `mirrors` | the source, and where else to find it |
| `sha256` | the source's, as published; empty: `sources.lock` |
| `top` | the folder the source unpacks into |
| `depends` | recipes built first, whose headers and libraries it uses |
| `keep` | what the package carries, relative to the prefix |
| `license` | the package's licence, for its manifest |
| `inputs` | files outside the recipe folder that decide what is built (in its key) |
| `recipe_patch` | in the source: changes for Android (paths) |
| `recipe_build` | configure, make, `make install DESTDIR="$dest"` |
| `recipe_after` | in what was installed: the last touches (links, files of ours) |

It is given `SQ_PREFIX` (the prefix), `SQ_HOST_TRIPLE` (`aarch64-linux-android`,
empty in a host test), `SQ_CROSS` (1 when what it builds cannot run here),
`CC` and the flags (the NDK's sysroot for API 28, 16 KB pages),
`CC_FOR_BUILD`, `JOBS`, `ROOT` (sqterm's folder), `src` and `dest`.

**API 28.** The packages are built for Android 9 and later (2026-11-15: the
Android 5.1 tablet is no longer a target). 28 is also sqterm's target SDK,
which it keeps so it may run programs from its own files.

## The packages

Each is a ustar archive compressed with zstd (a 1 MB window: what the app's
decoder holds while it unpacks), its files' paths relative to the prefix,
sorted and dated at the epoch, so two builds of the same source give the
same bytes. Executables are stripped. The index, `sqterm packages 1` and a
line a package (`name version file sha`), is what the app compares with the
one it last installed: when they differ, it unpacks them all again.

