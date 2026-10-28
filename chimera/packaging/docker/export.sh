#!/usr/bin/env bash
# Export a tested image for transfer to a machine without a published registry.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
die() { printf 'error: %s\n' "$*" >&2; exit 1; }
server=10.11
arch=amd64
output="$HERE/../dist/images"
while (($#)); do
  case $1 in
    --server) server=${2:?missing server}; shift 2 ;;
    --arch) arch=${2:?missing architecture}; shift 2 ;;
    --output) output=${2:?missing output directory}; shift 2 ;;
    -h|--help) echo 'usage: export.sh --server 10.11|11.8 [--arch amd64|arm64] [--output DIR]'; exit 0 ;;
    *) die "unknown argument '$1'" ;;
  esac
done
[[ $server == 10.11 || $server == 11.8 ]] || die 'server must be 10.11 or 11.8'
[[ $arch == amd64 || $arch == arm64 ]] || die 'arch must be amd64 or arm64'
version=$(cat "$ROOT/chimera/VERSION")
image="chimeradb:$version-$server-$arch"
[[ $(docker image inspect --format '{{.Architecture}}' "$image") == "$arch" ]] || die 'image architecture does not match'
mkdir -p "$output"
archive="chimeradb-$version-$server-$arch.tar.gz"
temporary=$(mktemp "$output/.export.XXXXXX")
trap 'rm -f "$temporary"' EXIT
docker image save --platform "linux/$arch" "$image" | gzip -n > "$temporary"
mv "$temporary" "$output/$archive"
(cd "$output" && shasum -a 256 "$archive" > "$archive.sha256")
printf 'Image archive: %s/%s\n' "$output" "$archive"
