#!/usr/bin/env bash
# Build a runnable image, sharing the Debian package build used by native installs.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
die() { printf 'error: %s\n' "$*" >&2; exit 1; }
series=10.11
case $(uname -m) in arm64|aarch64) arch=arm64 ;; *) arch=amd64 ;; esac
tag=
packages=
while (($#)); do
  case $1 in
    --server|--series) series=${2:?missing server series}; shift 2 ;;
    --arch) arch=${2:?missing architecture}; shift 2 ;;
    --tag) tag=${2:?missing image tag}; shift 2 ;;
    --packages) packages=${2:?missing package directory}; shift 2 ;;
    -h|--help)
      echo 'usage: build.sh --server 10.11|11.8 [--arch arm64|amd64] [--tag IMAGE] [--packages DIR]'
      exit 0 ;;
    *) die "unknown argument '$1'" ;;
  esac
done
[[ $series == 10.11 || $series == 11.8 ]] || die 'server must be 10.11 or 11.8'
[[ $arch == arm64 || $arch == amd64 ]] || die 'arch must be arm64 or amd64'
docker info >/dev/null 2>&1 || die 'Docker is not running; start your container engine'
[[ -n $tag ]] || tag="chimeradb:$(cat "$ROOT/chimera/VERSION")-$series-$arch"
if [[ -z $packages ]]; then
  "$HERE/../deb/build.sh" --server "$series" --arch "$arch"
  packages="$HERE/../dist/debian/$series/$arch"
fi
[[ -f $packages/SHA256SUMS && -f $packages/build-info.txt ]] || die 'missing package checksums/build-info.txt'
grep -Fxq 'Distribution: Debian 12 (bookworm)' "$packages/build-info.txt" || die 'the runtime image requires Debian 12 (bookworm) packages'
grep -qx "MariaDB series: $series" "$packages/build-info.txt" || die 'packages were built for another server series'
grep -qx "Architecture: $arch" "$packages/build-info.txt" || die 'packages were built for another architecture'
release=$(cat "$ROOT/chimera/VERSION")
package_version=$(sed -n 's/^ChimeraDB: //p' "$packages/build-info.txt")
[[ ${package_version%-*} == "$release" ]] || die 'packages were built for another ChimeraDB version'
context=$(mktemp -d "${TMPDIR:-/tmp}/chimera-image.XXXXXX")
trap 'rm -rf "$context"' EXIT
mkdir "$context/packages"
cp "$packages/"*.deb "$packages/SHA256SUMS" "$context/packages/"
cp "$HERE/runtime.Dockerfile" "$context/Dockerfile"
cp "$HERE/entrypoint.sh" "$HERE/70-chimera-container.cnf" "$context/"
cp "$HERE/../deb/configure-repository.sh" "$context/"
docker buildx build --platform "linux/$arch" --build-arg "SERIES=$series" \
  --load --tag "$tag" "$context"
printf 'Built %s\n' "$tag"
