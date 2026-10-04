#!/usr/bin/env bash
# Export committed ChimeraDB source for release assets and Homebrew formulae.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
die() { printf 'source archive: %s\n' "$*" >&2; exit 1; }
ref=HEAD
output="$HERE/dist/source"
while (($#)); do
  case $1 in
    --ref) ref=${2:?missing Git revision}; shift 2 ;;
    --output) output=${2:?missing output directory}; shift 2 ;;
    -h|--help) echo 'usage: source.sh [--ref COMMIT_OR_TAG] [--output DIR]'; exit 0 ;;
    *) die "unknown argument '$1'" ;;
  esac
done
commit=$(git -C "$ROOT" rev-parse --verify "$ref^{commit}")
version=$(git -C "$ROOT" show "$commit:chimera/VERSION")
[[ $version =~ ^[0-9]+\.[0-9]+\.[0-9]+([.-][0-9A-Za-z.-]+)?$ ]] || die 'invalid committed ChimeraDB version'
mkdir -p "$output"
archive="chimeradb-$version.tar.gz"
temporary=$(mktemp "$output/.source.XXXXXX")
trap 'rm -f "$temporary"' EXIT
# Only tracked ChimeraDB files and the project README. No upstream server trees,
# database volumes, ignored runtime state, or uncommitted working-tree files.
git -C "$ROOT" archive --format=tar --prefix="chimeradb-$version/" \
  "$commit" -- chimera README.md | gzip -n > "$temporary"
mv "$temporary" "$output/$archive"
(cd "$output" && shasum -a 256 "$archive" > "$archive.sha256")
printf 'ChimeraDB: %s\nGit commit: %s\n' "$version" "$commit" > "$output/$archive.build-info.txt"
printf 'Source archive: %s/%s\nGit commit: %s\n' "$output" "$archive" "$commit"
