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
#   tools/fetch_packages.sh --get NAME [DIR]   a large package (not in the APK),
#                                              into DIR (default
#                                              ~/storage/downloads, Termux's
#                                              link to Download): then in
#                                              sqterm, pkg install it --consume
#   tools/fetch_packages.sh --dry-run [...]    say what it would download
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
need() { command -v "$1" >/dev/null 2>&1 || fail "$1 is missing (Termux: pkg install $2)"; }
get() { curl -fsSL --retry 3 --connect-timeout 20 -o "$1" "$2"; }

MODE=release
DRY=0
names=()
while [ $# -gt 0 ]; do
  case $1 in
    --sources) MODE=sources ;;
    --get) MODE=get ;;
    --dry-run) DRY=1 ;;
    -h | --help) sed -n '2,34p' "$0"; exit 0 ;;
    -*) fail "unknown option: $1 (see --help)" ;;
    *) # After --get NAME, a second word is the folder.
       if [ "$MODE" = get ] && [ ${#names[@]} -eq 1 ]; then names+=("$1"); shift; continue; fi
       [ -f "$RECIPES/$1/recipe.sh" ] || fail "no recipe: packages/$1/recipe.sh"
       names+=("$1") ;;
  esac
  shift
done
if [ "$MODE" = get ]; then
  [ ${#names[@]} -ge 1 ] && [ ${#names[@]} -le 2 ] || fail "--get NAME [DIR]"
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
    [ "$bundled" = no ] || wanted+=("$name $version $key")
  done < <(bash "$RECIPES/build.sh" --keys)
  say release "packages ($SQ_PACKAGES_URL)"
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

# A large package (bundled=no, not in the APK), downloaded into DIR for pkg
# to install: in sqterm, pkg install DIR/<file> --consume.
fetch_one() {
  local want=$1 dir=$2 name version key bundled base=$SQ_PACKAGES_URL/packages asset
  while read -r name version key bundled; do
    [ "$name" = "$want" ] || continue
    asset=$name-$version-$key.tar.zst
    say release "packages ($SQ_PACKAGES_URL)"
    if [ "$DRY" = 1 ]; then say would "$asset into $dir"; return; fi
    mkdir -p "$dir"
    say download "$asset"
    get "$dir/$asset.part" "$base/$asset" && get "$dir/$asset.sha256" "$base/$asset.sha256" || {
      rm -f "$dir/$asset.part" "$dir/$asset.sha256"
      fail "no $asset in the release: CI builds it when its recipe reaches main"; }
    mv "$dir/$asset.part" "$dir/$asset"
    ( cd "$dir" && sha256sum -c --quiet "$asset.sha256" ) || fail "$asset does not match its .sha256"
    rm -f "$dir/$asset.sha256"
    say checked "$dir/$asset"
    say next "in sqterm: pkg install $name --consume (pkg finds the file in Download)"
    return
  done < <(bash "$RECIPES/build.sh" --keys)
  fail "no recipe: packages/$want"
}

# A git source (a recipe's git= and tag=): a shallow clone at the tag, in
# build/packages/sources/NAME-TAG.git. Its commit is checked against
# packages/sources.lock ("NAME@TAG COMMIT"), or recorded there on the first
# fetch, as a sha256 is for a file (SQ_LOCKED=1 refuses to record).
fetch_git() {
  local name=$1 repo=$2 tag=$3 dir expected actual
  need git git
  dir=$SOURCES/$name-$tag.git
  expected=$(awk -v g="$name@$tag" '$1 == g { print $2 }' "$LOCK" 2>/dev/null)
  if [ -d "$dir/.git" ] && [ -n "$expected" ] && [ "$(git -C "$dir" rev-parse HEAD)" = "$expected" ]; then
    say have "$name@$tag"
    return
  fi
  if [ "$DRY" = 1 ]; then say would "$name@$tag ($repo)"; return; fi
  rm -rf "$dir"
  mkdir -p "$SOURCES"
  say clone "$repo $tag"
  git -c advice.detachedHead=false clone -q --depth 1 --branch "$tag" "$repo" "$dir" ||
    fail "$name: could not clone $repo at $tag"
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
  local name=$1 version= url= mirrors= sha256= file= git= tag= from got=0 actual expected
  # The recipe's source fields only; its functions are not run.
  . "$RECIPES/$name/recipe.sh"
  if [ -n "$git" ]; then fetch_git "$name" "$git" "$tag"; return; fi
  if [ -z "$file" ]; then say local "$name (built from this repository)"; return; fi
  local path=$SOURCES/$file
  expected=$sha256
  if [ -z "$expected" ] && [ -f "$LOCK" ]; then
    expected=$(awk -v f="$file" '$1 == f { print $2 }' "$LOCK")
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
  fetch_one "${names[0]}" "${names[1]:-${HOME}/storage/downloads}"
else
  [ ${#names[@]} -gt 0 ] || read -r -a names <<< "$(bash "$RECIPES/build.sh" --list | awk '{ printf "%s ", $1 }')"
  for name in "${names[@]}"; do fetch_source "$name"; done
  [ "$DRY" = 1 ] || say next "make packages (offline)"
fi
