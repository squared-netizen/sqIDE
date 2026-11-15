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
    --dry-run) DRY=1 ;;
    -h | --help) sed -n '2,34p' "$0"; exit 0 ;;
    -*) fail "unknown option: $1 (see --help)" ;;
    *) [ -f "$RECIPES/$1/recipe.sh" ] || fail "no recipe: packages/$1/recipe.sh"
       names+=("$1") ;;
  esac
  shift
done
[ ${#names[@]} -eq 0 ] || [ "$MODE" = sources ] || fail "naming a package fetches its source: --sources ${names[*]}"
need curl curl
need sha256sum coreutils

fetch_release() {
  local base=$SQ_PACKAGES_URL/packages staging name version key asset
  local -a wanted=()
  while read -r name version key; do wanted+=("$name $version $key"); done \
    < <(bash "$RECIPES/build.sh" --keys)
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

fetch_source() {
  local name=$1 version= url= mirrors= sha256= file= from got=0 actual expected
  # The recipe's source fields only; its functions are not run.
  . "$RECIPES/$name/recipe.sh"
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
else
  [ ${#names[@]} -gt 0 ] || read -r -a names <<< "$(bash "$RECIPES/build.sh" --list | awk '{ printf "%s ", $1 }')"
  for name in "${names[@]}"; do fetch_source "$name"; done
  [ "$DRY" = 1 ] || say next "make packages (offline)"
fi
