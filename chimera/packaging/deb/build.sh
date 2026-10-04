#!/usr/bin/env bash
# Build Debian 12 packages using exactly the same Dockerfile locally and in CI.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
die() { printf 'error: %s\n' "$*" >&2; exit 1; }
usage() {
  echo 'usage: build.sh --series 10.11|11.8 [--arch amd64|arm64|both] [--mariadb-version VERSION] [--revision N] [--output DIR] [--jobs N]'
}
series=10.11
case $(uname -m) in arm64|aarch64) arch=arm64 ;; *) arch=amd64 ;; esac
version=
revision=1
output="$ROOT/chimera/packaging/dist/debian"
jobs=4
while (($#)); do
  case $1 in
    --series|--server) series=${2:?missing series}; shift 2 ;;
    --arch) arch=${2:?missing architecture}; shift 2 ;;
    --mariadb-version) version=${2:?missing version}; shift 2 ;;
    --revision) revision=${2:?missing revision}; shift 2 ;;
    --output) output=${2:?missing output directory}; shift 2 ;;
    --jobs) jobs=${2:?missing job count}; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown argument '$1'" ;;
  esac
done
[[ $series == 10.11 || $series == 11.8 ]] || die 'series must be 10.11 or 11.8'
[[ $arch == amd64 || $arch == arm64 || $arch == both ]] || die 'arch must be amd64, arm64 or both'
[[ $jobs =~ ^[1-9][0-9]*$ ]] || die 'jobs must be a positive integer'
[[ $revision =~ ^[1-9][0-9]*$ ]] || die 'revision must be a positive integer'
command -v docker >/dev/null || die 'Docker with buildx is required'
docker info >/dev/null || die 'cannot reach Docker; check the active context, socket permissions and container engine'
docker buildx version >/dev/null
epoch=${SOURCE_DATE_EPOCH:-$(git -C "$ROOT" log -1 --format=%ct 2>/dev/null || echo 1786320000)}
architectures=($arch)
[[ $arch != both ]] || architectures=(arm64 amd64)
for architecture in "${architectures[@]}"; do
  destination="$output/$series/$architecture"
  # BuildKit's local exporter merges into its destination. Export to an empty
  # directory so a new package revision cannot mix with old .debs that the
  # install commands would pick up with their wildcard.
  staging=$(mktemp -d "${TMPDIR:-/tmp}/chimera-packages.XXXXXX")
  trap '[[ -z ${staging:-} ]] || rm -rf "$staging"' EXIT
  docker buildx build --platform "linux/$architecture" \
    --file "$HERE/Dockerfile" --target packages \
    --build-arg "SERIES=$series" --build-arg "MARIADB_VERSION=$version" \
    --build-arg "REVISION=$revision" --build-arg "JOBS=$jobs" \
    --build-arg "SOURCE_DATE_EPOCH=$epoch" \
    --output "type=local,dest=$staging" "$ROOT"
  [[ -f $staging/SHA256SUMS && -f $staging/build-info.txt ]] || die 'package export is incomplete'
  packages=("$staging"/chimeradb*.deb)
  [[ -f ${packages[0]} ]] || die 'package export contains no ChimeraDB packages'
  # Preserve the last successful build if Docker fails. Only replace our own
  # generated package files after the new export is complete; leave unrelated
  # files in a user-selected output directory alone.
  mkdir -p "$destination"
  for previous in "$destination"/chimeradb*.deb; do
    [[ ! -f $previous ]] || rm -f -- "$previous"
  done
  cp -p "${packages[@]}" "$staging/SHA256SUMS" "$staging/build-info.txt" "$destination/"
  rm -rf "$staging"
  staging=
  printf 'Packages: %s\n' "$destination"
done
