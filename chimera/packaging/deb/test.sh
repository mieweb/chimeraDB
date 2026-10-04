#!/usr/bin/env bash
# Install, reinstall and purge in a disposable Debian container. Never uses a
# host database or mounts any host data directory.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
series=10.11
case $(uname -m) in arm64|aarch64) arch=arm64 ;; *) arch=amd64 ;; esac
packages= previous_packages=
while (($#)); do
  case $1 in
    --series|--server) series=${2:?missing series}; shift 2 ;;
    --arch) arch=${2:?missing architecture}; shift 2 ;;
    --packages) packages=${2:?missing package directory}; shift 2 ;;
    --previous-packages) previous_packages=${2:?missing previous package directory}; shift 2 ;;
    -h|--help) echo 'usage: test.sh --series 10.11|11.8 --arch amd64|arm64 [--packages DIR] [--previous-packages DIR]'; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 1 ;;
  esac
done
[[ $series == 10.11 || $series == 11.8 ]]
[[ $arch == arm64 || $arch == amd64 ]]
packages=${packages:-$ROOT/chimera/packaging/dist/debian/$series/$arch}
packages=$(cd "$packages" && pwd)
test -f "$packages/SHA256SUMS"
previous_mount=()
if [[ -n $previous_packages ]]; then
  previous_packages=$(cd "$previous_packages" && pwd)
  test -f "$previous_packages/SHA256SUMS"
  previous_mount=(--mount "type=bind,src=$previous_packages,dst=/previous-packages,readonly")
fi
tag="chimeradb-package-test:$series-$arch"
docker buildx build --platform "linux/$arch" --load --target repository \
  --build-arg "SERIES=$series" --file "$HERE/Dockerfile" --tag "$tag" "$ROOT"
docker run --rm --platform "linux/$arch" \
  "${previous_mount[@]+"${previous_mount[@]}"}" \
  --mount "type=bind,src=$packages,dst=/packages,readonly" \
  --mount "type=bind,src=$HERE/test-inside.sh,dst=/test.sh,readonly" \
  --env "SERIES=$series" --env "ARCH=$arch" "$tag" bash /test.sh
