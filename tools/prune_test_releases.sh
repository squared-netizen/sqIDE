#!/usr/bin/env bash
# prune_test_releases.sh: delete sqterm's old test releases (test-<commit>),
# keeping the newest.
#
#   prune_test_releases.sh            list what would go (nothing is changed)
#   prune_test_releases.sh --delete   delete those releases and their tags
#   prune_test_releases.sh --keep 3   keep the newest 3 (default 1)
#
# Only bash and curl: runs in sqterm as it comes. The token is read from
# ~/.git-credentials (the https://USER:TOKEN@github.com line git uses), or
# $GITHUB_TOKEN, and goes to curl on its standard input, so it is never on a
# command line or printed. Deleting needs a token that can write the
# repository's contents. Environment: SQ_RELEASES_REPO (default
# squared-netizen/sqterm).
set -euo pipefail

REPO=${SQ_RELEASES_REPO:-squared-netizen/sqterm}
API=https://api.github.com/repos/$REPO
KEEP=1
DELETE=0

fail() { printf 'prune_test_releases: %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
  case $1 in
    --delete) DELETE=1 ;;
    --keep) shift; [[ ${1:-} =~ ^[0-9]+$ ]] || fail "--keep needs a number"; KEEP=$1 ;;
    -h | --help)
      while IFS= read -r line; do
        [[ $line == '#!'* ]] && continue
        [[ $line == '#'* ]] || break
        line=${line#'#'}
        printf '%s\n' "${line# }"
      done < "${BASH_SOURCE[0]}"
      exit 0 ;;
    *) fail "unknown option: $1 (see --help)" ;;
  esac
  shift
done
command -v curl >/dev/null 2>&1 || fail "curl is missing"

token=${GITHUB_TOKEN:-}
if [ -z "$token" ] && [ -r "$HOME/.git-credentials" ]; then
  while IFS= read -r line; do
    if [[ $line =~ ^https://[^:/@]+:([^@]+)@github\.com/?$ ]]; then token=${BASH_REMATCH[1]}; break; fi
  done < "$HOME/.git-credentials"
fi
[ "$DELETE" = 0 ] || [ -n "$token" ] || fail "no token: none in ~/.git-credentials and no \$GITHUB_TOKEN"

# curl with the token as a header from standard input (never in argv).
api() {
  local method=$1 url=$2
  {
    [ -z "$token" ] || printf 'header = "Authorization: Bearer %s"\n' "$token"
    printf 'header = "Accept: application/vnd.github+json"\n'
  } | curl -fsS -K - -X "$method" "$url"
}

# Every release: each release's tag_name comes before its published_at in
# GitHub's JSON, and assets have neither, so each pair is one release.
declare -a tags=() dates=()
page=1
while :; do
  body=$(api GET "$API/releases?per_page=100&page=$page") || fail "cannot list $REPO's releases"
  tag=''
  found=0
  while [[ $body =~ \"(tag_name|published_at)\":\ *(\"([^\"]*)\"|null)(.*) ]]; do
    body=${BASH_REMATCH[4]}
    if [ "${BASH_REMATCH[1]}" = tag_name ]; then
      tag=${BASH_REMATCH[3]}
      found=1
    elif [ -n "$tag" ]; then
      tags+=("$tag"); dates+=("${BASH_REMATCH[3]:-0}")
      tag=''
    fi
  done
  [ "$found" = 1 ] || break
  page=$((page + 1))
done

# The test releases, newest first (ISO dates sort as text).
declare -a test=()
for i in "${!tags[@]}"; do
  [[ ${tags[$i]} == test-* ]] && test+=("${dates[$i]} ${tags[$i]}")
done
[ ${#test[@]} -gt 0 ] || { echo "no test releases in $REPO"; exit 0; }
mapfile -t test < <(printf '%s\n' "${test[@]}" | sort -r)

n=0
for entry in "${test[@]}"; do
  date=${entry%% *} tag=${entry#* }
  n=$((n + 1))
  if [ "$n" -le "$KEEP" ]; then
    printf '  keep    %s  %s\n' "$tag" "$date"
    continue
  fi
  if [ "$DELETE" = 0 ]; then
    printf '  delete  %s  %s\n' "$tag" "$date"
    continue
  fi
  release=$(api GET "$API/releases/tags/$tag") || fail "cannot read $tag"
  [[ $release =~ /releases/([0-9]+)\" ]] || fail "no id for $tag"
  api DELETE "$API/releases/${BASH_REMATCH[1]}" >/dev/null || fail "cannot delete release $tag"
  api DELETE "$API/git/refs/tags/$tag" >/dev/null || fail "release $tag deleted; its tag was not"
  printf '  deleted %s  %s\n' "$tag" "$date"
done
[ "$DELETE" = 1 ] || echo "(nothing changed: --delete to delete)"
