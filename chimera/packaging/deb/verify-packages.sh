#!/usr/bin/env bash
# Validate the complete install set before APT sees any of it. stdout contains
# either its version or exactly the three runtime package paths, one per line.
set -euo pipefail
die() { printf 'package validation: %s\n' "$*" >&2; exit 1; }
[[ $# -ge 4 ]] ||
  die 'usage: verify-packages.sh DIRECTORY SERIES ARCH "Debian VERSION (CODENAME)" [--runtime-files] [--allow-legacy-common-all]'
directory=$(cd "$1" && pwd)
series=$2 arch=$3 distribution=$4
shift 4
runtime=false
legacy_common_all=false
while (($#)); do
  case $1 in
    --runtime-files) $runtime && die 'duplicate --runtime-files'; runtime=true ;;
    --allow-legacy-common-all)
      $legacy_common_all && die 'duplicate --allow-legacy-common-all'
      legacy_common_all=true ;;
    *) die "unknown option: $1" ;;
  esac
  shift
done
[[ $series == 10.11 || $series == 11.8 ]] || die 'unsupported MariaDB series'
[[ $arch == amd64 || $arch == arm64 ]] || die 'unsupported architecture'
[[ -f $directory/SHA256SUMS && -f $directory/build-info.txt ]] || die 'missing checksums/build metadata'
shopt -s nullglob
files=("$directory"/*.deb)
(( ${#files[@]} >= 3 && ${#files[@]} <= 4 )) || die 'expected three runtime packages and optionally one debug package'
for file in "${files[@]}"; do
  [[ -f $file && ! -L $file ]] || die "not a regular package file: $file"
  [[ ${file##*/} =~ ^[a-zA-Z0-9.+:~_-]+\.deb$ ]] || die "unexpected package filename: $file"
done
# Compare the entire manifest with hashes of the entire input set. Unlike
# sha256sum --check alone, this rejects unlisted .debs, duplicate manifest rows,
# missing files and paths outside this directory, without reading those paths.
expected=$(LC_ALL=C sort "$directory/SHA256SUMS")
actual=$(cd "$directory" && sha256sum ./*.deb | LC_ALL=C sort)
[[ $expected == "$actual" ]] || die 'checksum manifest does not exactly cover the package set'
metadata() { sed -n "s/^$1: //p" "$directory/build-info.txt"; }
[[ $(metadata 'MariaDB series') == "$series" ]] || die 'wrong MariaDB series in build metadata'
[[ $(metadata Architecture) == "$arch" ]] || die 'wrong architecture in build metadata'
[[ $(metadata Distribution) == "$distribution" ]] || die 'wrong distribution in build metadata'
version=$(metadata ChimeraDB)
server_version=$(metadata 'MariaDB package')
[[ -n $version ]] || die 'missing ChimeraDB version in build metadata'
[[ -n $server_version && ${server_version#*:} == "$series".* ]] || die 'invalid MariaDB package version'
common=0 meta=0 plugin=0 debug=0
runtime_files=()
for file in "${files[@]}"; do
  package=$(dpkg-deb -f "$file" Package)
  package_arch=$(dpkg-deb -f "$file" Architecture)
  [[ $(dpkg-deb -f "$file" Version) == "$version" ]] || die "mixed or incorrect package version: $file"
  case $package in
    chimeradb)
      meta=$((meta + 1)); [[ $package_arch == all ]] || die "wrong metapackage architecture: $file" ;;
    chimeradb-common)
      common=$((common + 1))
      # Only explicitly selected previous releases may predate the compiled
      # health helper's move into the architecture-specific common package.
      [[ $package_arch == "$arch" || ( $legacy_common_all == true && $package_arch == all ) ]] ||
        die "wrong common package architecture: $file"
      ;;
    "chimeradb-plugin-$series")
      plugin=$((plugin + 1)); [[ $package_arch == "$arch" ]] || die "wrong plugin architecture: $file"
      dependencies=$(dpkg-deb -f "$file" Depends)
      printf '%s\n' "$dependencies" | tr ',' '\n' | sed 's/^[[:space:]]*//;s/[[:space:]]*$//' |
        grep -Fxq "mariadb-server (= $server_version)" || die 'plugin does not pin the recorded MariaDB package'
      ;;
    "chimeradb-plugin-$series-dbgsym")
      debug=$((debug + 1)); [[ $package_arch == "$arch" ]] || die "wrong debug package architecture: $file"
      continue ;;
    *) die "unexpected package: $package" ;;
  esac
  runtime_files+=("$file")
done
[[ $meta == 1 && $common == 1 && $plugin == 1 && $debug -le 1 ]] || die 'missing or duplicate package identity'
if $runtime; then
  printf '%s\n' "${runtime_files[@]}"
else
  printf '%s\n' "$version"
fi
