#!/usr/bin/env bash
# tools/prune_release.sh: list, or delete, the archives in the `packages`
# release that no recipe asks for any more.
#
#   tools/prune_release.sh             list them (nothing is changed)
#   tools/prune_release.sh --delete    delete them, and their .sha256
#
# CI adds an archive to the release for every key a package has had
# (<name>-<version>-<key>.tar.zst), and never removes one: a recipe changed
# ten times leaves ten. This keeps those the recipes in this folder ask for
# (`bash packages/build.sh --keys`), as tarballs and as cartridges (.sq), and
# the `index` and `index-3` pkg reads, and nothing
# else, so run it from an up-to-date main: a checkout that is behind would
# delete what main needs,
# and one that is ahead would keep what main does not. It refuses to delete
# unless HEAD is origin/main as last fetched.
#
# A checkout older than main asks for keys that are gone once this has run:
# git pull, then tools/fetch_packages.sh again.
#
# Needs gh (the GitHub CLI), signed in with write access to the repository
# for --delete (Termux: pkg install gh; gh auth login). Environment:
# SQ_PACKAGES_REPO (default squared-netizen/sqterm).
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
SQ_PACKAGES_REPO=${SQ_PACKAGES_REPO:-squared-netizen/sqterm}
RELEASE=packages

say() { printf '  %-14s %s\n' "$1" "$2"; }
fail() { printf 'prune_release: %s\n' "$*" >&2; exit 1; }

DELETE=0
case ${1:-} in
  '') ;;
  --delete) DELETE=1 ;;
  -h | --help) sed -n '2,22p' "$0"; exit 0 ;;
  *) fail "unknown option: $1 (see --help)" ;;
esac
command -v gh >/dev/null 2>&1 || fail "gh is missing (Termux: pkg install gh; gh auth login)"

if [ "$DELETE" = 1 ]; then
  head=$(git -C "$ROOT" rev-parse HEAD)
  main=$(git -C "$ROOT" rev-parse origin/main 2>/dev/null) || fail "no origin/main: git fetch first"
  [ "$head" = "$main" ] || fail "HEAD is not origin/main: check out main and git pull first"
  [ -z "$(git -C "$ROOT" status --porcelain -- packages)" ] || fail "packages/ has changes not on main"
fi

# What the recipes ask for: <name>-<version>-<key>.tar.zst, and the same
# package as a cartridge, .sq (the release carries both, 2026-11-19).
declare -A wanted=()
while read -r name version key _; do
  wanted["$name-$version-$key.tar.zst"]=1
  wanted["$name-$version-$key.sq"]=1
done < <(bash "$ROOT/packages/build.sh" --keys)
[ ${#wanted[@]} -gt 0 ] || fail "no keys from packages/build.sh --keys"

# What the release has: id and name of each asset.
assets=$(gh api --paginate "repos/$SQ_PACKAGES_REPO/releases/tags/$RELEASE" \
           --jq '.assets[] | "\(.id) \(.name)"') || fail "cannot read the release $RELEASE"

kept=0 stale=0
while read -r id asset; do
  [ -n "$id" ] || continue
  archive=${asset%.sha256}
  # The indexes pkg reads (CI rewrites them on every run on main): index
  # for the tarballs, index-3 for the cartridges.
  if [ -n "${wanted[$archive]:-}" ] || [ "$asset" = index ] || [ "$asset" = index-3 ]; then
    kept=$((kept + 1))
    continue
  fi
  stale=$((stale + 1))
  if [ "$DELETE" = 1 ]; then
    gh api -X DELETE "repos/$SQ_PACKAGES_REPO/releases/assets/$id" > /dev/null
    say deleted "$asset"
  else
    say stale "$asset"
  fi
done <<< "$assets"

if [ "$DELETE" = 1 ]; then
  say done "$stale deleted, $kept kept"
else
  say total "$stale stale, $kept kept"
  [ "$stale" -eq 0 ] || say next "tools/prune_release.sh --delete"
fi
