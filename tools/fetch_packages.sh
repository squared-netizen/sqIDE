#!/usr/bin/env bash
# tools/fetch_packages.sh: the one network step for sqterm's packages (12.5).
# Run on purpose, when installing or after a recipe changes; no build calls
# it. Everything after it (make packages, make apk) is offline.
#
#   tools/fetch_packages.sh                    the packages CI built for these
#                                              recipes, into
#                                              sq_android/assets/packages/
#   tools/fetch_packages.sh --sources [name...]  the recipes' sources, into
#                                              build/packages/sources/, to
#                                              build here: then make packages
#   tools/fetch_packages.sh --get NAME...      packages, and what each needs at
#                                              run time (packages/needs), into
#                                              sqterm's packages folder
#                                              (SQ_PKG_DIR, default
#                                              ~/storage/shared/sqterm/packages,
#                                              /sdcard/sqterm/packages), with
#                                              an index pkg reads: then in
#                                              sqterm, pkg install NAME, or
#                                              pkg upgrade, offline
#   tools/fetch_packages.sh --dry-run [...]    say what it would download
#   tools/fetch_packages.sh --bundle SET       which packages go in the APK
#                                              (SQ_BUNDLE): default, the
#                                              bundled=yes ones; basic, zsh
#                                              and pkg (and ncurses, their
#                                              terminfo); all, every package
#                                              (about 110 MB)
#
# The packages: CI (.github/workflows/packages.yml) builds each package once
# per key (`bash packages/build.sh --keys`: a sha256 of its recipe, what it
# depends on and the API level) and adds it to the GitHub release named
# `packages` as <name>-<version>-<key>.tar.zst, with a .sha256 beside it. This
# downloads the one for each recipe as it is in this folder, checks each
# against its .sha256, and only once all have arrived and matched replaces
# what was in sq_android/assets/packages/ and writes the index. The
# repository is public: no token. Those sha256s guard against a broken
# download; they come from the same release, so they are not a signature. A
# recipe changed here has no archive until CI builds it from main: fetch its
# sources and build it here instead.
#
# The sources: each recipe's url (or its mirrors), checked against the
# recipe's sha256, or packages/sources.lock for a source that publishes none
# (ncurses). The first download of such a source is recorded there: commit
# it. With SQ_LOCKED=1 (CI) an unrecorded source fails instead.
#
# Environment: SQ_PACKAGES_REPO (default squared-netizen/sqterm), or
# SQ_PACKAGES_URL for the whole of it (a mirror, or a test):
# <url>/packages/<file>.
#
# Runs from anywhere: the repo is found from this script's own path. Needs
# curl and sha256sum (Termux: pkg install curl).
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
RECIPES=$ROOT/packages
SOURCES=$ROOT/build/packages/sources
LOCK=$RECIPES/sources.lock
OUT=$ROOT/sq_android/assets/packages
SQ_PACKAGES_REPO=${SQ_PACKAGES_REPO:-squared-netizen/sqterm}
SQ_PACKAGES_URL=${SQ_PACKAGES_URL:-https://github.com/$SQ_PACKAGES_REPO/releases/download}

say() { printf '  %-14s %s\n' "$1" "$2"; }
fail() { printf 'fetch_packages: %s\n' "$*" >&2; exit 1; }

# The second word of each packages/sources.lock line whose first word is $1.
lock_value() {
  local key value _
  [ -f "$LOCK" ] || return 0
  while read -r key value _; do
    if [ "$key" = "$1" ]; then echo "$value"; fi
  done < "$LOCK"
}
need() { command -v "$1" >/dev/null 2>&1 || fail "$1 is missing (Termux: pkg install $2)"; }
get() { curl -fsSL --retry 3 --connect-timeout 20 -o "$1" "$2"; }

MODE=release
DRY=0
BUNDLE=${SQ_BUNDLE:-default}
names=()
while [ $# -gt 0 ]; do
  case $1 in
    --sources) MODE=sources ;;
    --get) MODE=get ;;
    --dry-run) DRY=1 ;;
    --bundle) [ $# -ge 2 ] || fail "--bundle default|basic|all"; BUNDLE=$2; shift ;;
    -h | --help) head -n 44 "$0" | tail -n +2; exit 0 ;;
    -*) fail "unknown option: $1 (see --help)" ;;
    *) [ -f "$RECIPES/$1/recipe.sh" ] || fail "no recipe: packages/$1/recipe.sh"
       names+=("$1") ;;
  esac
  shift
done
case $BUNDLE in default | basic | all) ;; *) fail "--bundle is default, basic or all, not $BUNDLE" ;; esac
if [ "$MODE" = get ]; then
  [ ${#names[@]} -ge 1 ] || fail "--get NAME..."
elif [ ${#names[@]} -gt 0 ] && [ "$MODE" != sources ]; then
  fail "naming a package fetches its source: --sources ${names[*]}"
fi
need curl curl
need sha256sum coreutils

fetch_release() {
  local base=$SQ_PACKAGES_URL/packages staging name version key asset
  local -a wanted=()
  local bundled
  while read -r name version key bundled; do
    case $BUNDLE in
      all) ;;
      basic) case $name in ncurses | zsh | pkg) ;; *) continue ;; esac ;;
      *) [ "$bundled" != no ] || continue ;;
    esac
    wanted+=("$name $version $key")
  done < <(bash "$RECIPES/build.sh" --keys)
  say release "packages ($SQ_PACKAGES_URL), bundle: $BUNDLE"
  if [ "$DRY" = 1 ]; then
    for line in "${wanted[@]}"; do read -r name version key <<< "$line"; say would "$name-$version-$key.tar.zst"; done
    return
  fi
  staging=$ROOT/build/packages/download
  rm -rf "$staging"
  mkdir -p "$staging"
  for line in "${wanted[@]}"; do
    read -r name version key <<< "$line"
    asset=$name-$version-$key.tar.zst
    say download "$asset"
    get "$staging/$asset" "$base/$asset" && get "$staging/$asset.sha256" "$base/$asset.sha256" || {
      rm -rf "$staging"
      fail "no $asset in the release: CI builds it when this recipe reaches main
          (github.com/$SQ_PACKAGES_REPO/actions). Or build here:
          tools/fetch_packages.sh --sources; make packages"; }
    ( cd "$staging" && sha256sum -c --quiet "$asset.sha256" ) || fail "$asset does not match its .sha256"
  done
  mkdir -p "$OUT"
  rm -f "$OUT"/*.tar.zst "$OUT/index"
  for line in "${wanted[@]}"; do
    read -r name version key <<< "$line"
    mv "$staging/$name-$version-$key.tar.zst" "$OUT/$name-$version.tar.zst"
  done
  rm -rf "$staging"
  bash "$RECIPES/build.sh" --index > /dev/null
  say packages "${#wanted[@]} in ${OUT#"$ROOT"/}, checked"
}

# What a package needs at run time (packages/needs), comma-separated, or -.
needs_of() {
  local n= key rest words
  if [ -f "$RECIPES/needs" ]; then
    while read -r key rest; do
      [ "$key" = "$1:" ] || continue
      read -r -a words <<< "$rest"
      n=$(IFS=,; echo "${words[*]}")
    done < "$RECIPES/needs"
  fi
  echo "${n:--}"
}

# Packages, and what they need, into sqterm's packages folder, each checked
# against its .sha256, and the folder's index (pkg's: "sqterm release index
# 2", a line a package) brought up to date for them: in sqterm, pkg install
# NAME and pkg upgrade find them there, offline, by name.
fetch_get() {
  local dir=${SQ_PKG_DIR:-$HOME/storage/shared/sqterm/packages} base=$SQ_PACKAGES_URL/packages
  local name version key bundled asset sha line
  local -A keys=() want=()
  while read -r name version key bundled; do keys[$name]="$version $key $bundled"; done \
    < <(bash "$RECIPES/build.sh" --keys)
  # The names asked for, and what they need, and what that needs.
  local -a todo=("${names[@]}") order=()
  while [ ${#todo[@]} -gt 0 ]; do
    name=${todo[0]}; todo=("${todo[@]:1}")
    [ -n "${want[$name]:-}" ] && continue
    want[$name]=1
    order+=("$name")
    line=$(needs_of "$name")
    [ "$line" = - ] || todo+=(${line//,/ })
  done
  say release "packages ($SQ_PACKAGES_URL)"
  if [ "$DRY" = 1 ]; then
    for name in "${order[@]}"; do read -r version key _ <<< "${keys[$name]}"; say would "$name-$version-$key.tar.zst into $dir"; done
    return
  fi
  mkdir -p "$dir" || fail "cannot make $dir (Termux: termux-setup-storage, once)"
  local lines=
  for name in "${order[@]}"; do
    read -r version key bundled <<< "${keys[$name]}"
    asset=$name-$version-$key.tar.zst
    if [ -f "$dir/$asset" ] && [ -f "$dir/$asset.sha256" ] &&
       ( cd "$dir" && sha256sum -c --quiet "$asset.sha256" ) 2>/dev/null; then
      say have "$asset"
    else
      say download "$asset"
      get "$dir/$asset.part" "$base/$asset" && get "$dir/$asset.sha256" "$base/$asset.sha256" || {
        rm -f "$dir/$asset.part" "$dir/$asset.sha256"
        fail "no $asset in the release: CI builds it when its recipe reaches main"; }
      mv "$dir/$asset.part" "$dir/$asset"
      ( cd "$dir" && sha256sum -c --quiet "$asset.sha256" ) || fail "$asset does not match its .sha256"
      say checked "$asset"
    fi
    sha=$(cut -d ' ' -f 1 "$dir/$asset.sha256")
    lines+="$name $version $key $bundled $sha $(needs_of "$name")"$'\n'
    # Older archives of it go: the index names this one.
    for old in "$dir/$name"-[0-9]*.tar.zst; do
      [ -e "$old" ] && [ "$old" != "$dir/$asset" ] && rm -f "$old" "$old.sha256"
    done
  done
  # The folder's index: these lines, then the others it had.
  {
    echo "sqterm release index 2"
    printf '%s' "$lines"
    if [ -f "$dir/index" ]; then
      # An index of version 1 has no needs: "-" for each.
      local v1=
      [ "$(head -n 1 "$dir/index")" = "sqterm release index 1" ] && v1=" -"
      tail -n +2 "$dir/index" | while read -r name rest; do
        if [ -n "$name" ] && [ -z "${want[$name]:-}" ]; then echo "$name $rest$v1"; fi
      done
    fi
  } > "$dir/index.new"
  mv "$dir/index.new" "$dir/index"
  say index "$dir/index"
  say next "in sqterm: pkg install ${names[*]} (or pkg upgrade), offline"
}

# A git source (a recipe's git= and tag=): a shallow clone at the tag, in
# build/packages/sources/NAME-TAG.git. Its commit is checked against
# packages/sources.lock ("NAME@TAG COMMIT"), or recorded there on the first
# fetch, as a sha256 is for a file (SQ_LOCKED=1 refuses to record).
# Folders after the tag make it a sparse clone of those folders alone, their
# files fetched as they are checked out (a recipe's `also`: a few folders of
# a large repository).
# fetch_commit DIR REPO COMMIT [FOLDER...]: DIR, a clone of only COMMIT
# (its folders alone when given), checked out; quiet, and false when the
# server would not serve it.
fetch_commit() {
  local dir=$1 repo=$2 commit=$3
  shift 3
  git init -q "$dir" &&
    git -C "$dir" remote add origin "$repo" &&
    if [ $# -gt 0 ]; then
      git -C "$dir" fetch -q --depth 1 --filter=blob:none origin "$commit" 2>/dev/null &&
        git -C "$dir" sparse-checkout set -- "$@"
    else
      git -C "$dir" fetch -q --depth 1 origin "$commit" 2>/dev/null
    fi &&
    git -c advice.detachedHead=false -C "$dir" checkout -q FETCH_HEAD 2>/dev/null &&
    [ "$(git -C "$dir" rev-parse HEAD)" = "$commit" ]
}

fetch_git() {
  local name=$1 repo=$2 tag=$3 dir expected actual
  shift 3
  need git git
  dir=$SOURCES/$name-$tag.git
  expected=$(lock_value "$name@$tag")
  if [ -d "$dir/.git" ] && [ -n "$expected" ] && [ "$(git -C "$dir" rev-parse HEAD)" = "$expected" ]; then
    # The folders a recipe asks for may have changed since the clone.
    if [ $# -gt 0 ] && [ "$DRY" != 1 ]; then
      git -C "$dir" sparse-checkout set -- "$@" || fail "$name: could not check out $* of $repo"
    fi
    say have "$name@$tag"
    return
  fi
  if [ "$DRY" = 1 ]; then say would "$name@$tag ($repo${*:+: $*})"; return; fi
  rm -rf "$dir"
  mkdir -p "$SOURCES"
  say clone "$repo $tag${*:+ ($*)}"
  # With the commit already in sources.lock, that commit itself, fetched
  # by its id: what the lock says is what arrives, and no annotated tag is
  # fetched (a shallow clone at one makes git say "... is not a commit!").
  # A server that serves no commit by id: the tag, as below.
  if [ -n "$expected" ] && fetch_commit "$dir" "$repo" "$expected" "$@"; then
    :
  elif [ $# -gt 0 ]; then
    rm -rf "$dir"
    git -c advice.detachedHead=false clone -q --depth 1 --filter=blob:none --sparse --branch "$tag" "$repo" "$dir" &&
      git -C "$dir" sparse-checkout set -- "$@" ||
      fail "$name: could not clone $* of $repo at $tag"
  else
    rm -rf "$dir"
    git -c advice.detachedHead=false clone -q --depth 1 --branch "$tag" "$repo" "$dir" ||
      fail "$name: could not clone $repo at $tag"
  fi
  actual=$(git -C "$dir" rev-parse HEAD)
  if [ -z "$expected" ] && [ "${SQ_LOCKED:-0}" = 1 ]; then
    rm -rf "$dir"
    fail "$name@$tag: no commit in packages/sources.lock, and SQ_LOCKED=1"
  elif [ -z "$expected" ]; then
    printf '%s %s\n' "$name@$tag" "$actual" >> "$LOCK"
    say recorded "$name@$tag $actual in packages/sources.lock (commit it)"
  elif [ "$actual" != "$expected" ]; then
    rm -rf "$dir"
    fail "$name@$tag: commit $actual, expected $expected (the tag moved?)"
  fi
  say checked "$name@$tag"
}

fetch_source() {
  local name=$1 version= url= mirrors= sha256= file= git= tag= also= extra= from got=0 actual expected
  local sub repo dirs
  # The recipe's source fields only; its functions are not run.
  . "$RECIPES/$name/recipe.sh"
  if [ -n "$git" ]; then
    fetch_git "$name" "$git" "$tag"
    # `also`: more repositories at the same tag, a line each: "SUB URL
    # FOLDER...", cloned as NAME.SUB.
    while read -r sub repo dirs; do
      [ -n "$sub" ] || continue
      # shellcheck disable=SC2086  # the folders, a word each
      fetch_git "$name.$sub" "$repo" "$tag" $dirs
    done <<< "$also"
    return
  fi
  if [ -z "$file" ]; then say local "$name (built from this repository)"; return; fi
  # `extra`: more source files, "SUB URL SHA256" a line, each checked by
  # the sha256 the recipe gives (packages/build.sh).
  local sub xurl xsha xpath
  while read -r sub xurl xsha; do
    [ -n "$sub" ] || continue
    xpath=$SOURCES/$name.$sub-${xurl##*/}
    if [ -f "$xpath" ] && [ "$(sha256sum "$xpath" | cut -d ' ' -f 1)" = "$xsha" ]; then
      say have "${xpath##*/}"; continue
    fi
    if [ "$DRY" = 1 ]; then say would "${xpath##*/} ($xurl)"; continue; fi
    mkdir -p "$SOURCES"
    say download "$xurl"
    get "$xpath.part" "$xurl" || fail "$name: could not download $xurl"
    [ "$(sha256sum "$xpath.part" | cut -d ' ' -f 1)" = "$xsha" ] ||
      { rm -f "$xpath.part"; fail "${xpath##*/}: not the sha256 the recipe gives"; }
    mv "$xpath.part" "$xpath"
    say checked "${xpath##*/}"
  done <<< "$extra"
  local path=$SOURCES/$file
  expected=$sha256
  if [ -z "$expected" ] && [ -f "$LOCK" ]; then
    expected=$(lock_value "$file")
  fi
  if [ -f "$path" ] && [ -n "$expected" ] &&
     [ "$(sha256sum "$path" | cut -d ' ' -f 1)" = "$expected" ]; then
    say have "$file"
    return
  fi
  if [ "$DRY" = 1 ]; then say would "$file ($url)"; return; fi
  mkdir -p "$SOURCES"
  for from in $url $mirrors; do
    say download "$from"
    if get "$path.part" "$from"; then got=1; break; fi
  done
  [ "$got" = 1 ] || fail "$name: could not download $file"
  actual=$(sha256sum "$path.part" | cut -d ' ' -f 1)
  if [ -z "$expected" ] && [ "${SQ_LOCKED:-0}" = 1 ]; then
    rm -f "$path.part"
    fail "$file: no sha256 in the recipe or packages/sources.lock, and SQ_LOCKED=1:
          fetch it on the phone once and commit packages/sources.lock"
  elif [ -z "$expected" ]; then
    printf '%s %s\n' "$file" "$actual" >> "$LOCK"
    say recorded "$file $actual in packages/sources.lock (commit it)"
  elif [ "$actual" != "$expected" ]; then
    rm -f "$path.part"
    fail "$file: sha256 $actual, expected $expected"
  fi
  mv "$path.part" "$path"
  say checked "$file"
}

if [ "$MODE" = release ]; then
  fetch_release
elif [ "$MODE" = get ]; then
  fetch_get
else
  [ ${#names[@]} -gt 0 ] || read -r -a names <<< "$(bash "$RECIPES/build.sh" --list | while read -r n _; do printf '%s ' "$n"; done)"
  for name in "${names[@]}"; do fetch_source "$name"; done
  [ "$DRY" = 1 ] || say next "make packages (offline)"
fi
