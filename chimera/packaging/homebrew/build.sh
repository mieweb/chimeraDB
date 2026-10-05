#!/usr/bin/env bash
# Build only the plugin through MariaDB's CMake, then stage the Homebrew payload.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
CHIMERA=$(cd "$HERE/../.." && pwd)
die() { printf 'homebrew build: %s\n' "$*" >&2; exit 1; }
server= source_dir= server_prefix= prefix= build_dir=
while (($#)); do
  case "$1" in
    --server) server=${2:?}; shift 2 ;;
    --mariadb-source) source_dir=${2:?}; shift 2 ;;
    --mariadb-prefix) server_prefix=${2:?}; shift 2 ;;
    --prefix) prefix=${2:?}; shift 2 ;;
    --build-dir) build_dir=${2:?}; shift 2 ;;
    *) die "unknown argument: $1" ;;
  esac
done
[[ $server == 10.11 || $server == 11.8 ]] || die '--server 10.11|11.8 is required'
[[ -f $source_dir/VERSION && -x $server_prefix/bin/mariadbd && -n $prefix && -n $build_dir ]] ||
  die '--mariadb-source, --mariadb-prefix, --prefix and --build-dir are required'
[[ $(uname -s) == Darwin ]] || die 'this is the macOS Homebrew build; use the Debian builder on Linux'
source_version=$(awk -F= '/^MYSQL_VERSION_(MAJOR|MINOR|PATCH)=/ {v=v sep $2; sep="."} END {print v}' "$source_dir/VERSION")
runtime_version=$("$server_prefix/bin/mariadbd" --version | sed -E 's/.*(Ver|Distrib) ([0-9]+\.[0-9]+\.[0-9]+).*/\2/')
[[ $source_version == "$server".* && $source_version == "$runtime_version" ]] ||
  die "MariaDB source $source_version does not match runtime $runtime_version ($server required)"
resolved_server_prefix=$(cd "$server_prefix" && pwd -P)

mkdir -p "$build_dir" "$prefix/lib/chimeradb/plugin" "$prefix/libexec" "$prefix/share/chimeradb/sql"
export CHIMERA_OUT=$build_dir
if [[ -n ${CHIMERA_HOMEBREW_PREFIX:-} && -n ${CHIMERA_BISON_PREFIX:-} && -n ${CHIMERA_OPENSSL_PREFIX:-} ]]; then
  # A formula's superenv need not expose the brew executable. Use the explicit
  # dependency prefixes it supplies instead of invoking Homebrew recursively.
  brew_prefix=$CHIMERA_HOMEBREW_PREFIX
  bison_prefix=$CHIMERA_BISON_PREFIX
  ssl_prefix=$CHIMERA_OPENSSL_PREFIX
elif command -v brew >/dev/null 2>&1; then
  brew_prefix=$(brew --prefix)
  bison_prefix=$(brew --prefix bison)
  ssl_prefix=$(brew --prefix openssl@3)
else
  die 'Homebrew is needed to locate the macOS build dependencies'
fi
export PATH="$bison_prefix/bin:$PATH"
export PKG_CONFIG_PATH="$brew_prefix/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
cmake -S "$CHIMERA/translator" -B "$build_dir/translator" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build "$build_dir/translator"

link="$source_dir/plugin/chimera_mongo"
[[ ! -e $link || -L $link ]] || die "refusing to overwrite $link"
ln -sfn "$CHIMERA/plugin/chimera_mongo" "$link"
sdk=$(xcrun --sdk macosx --show-sdk-path)
cmake -S "$source_dir" -B "$build_dir/server" -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DCMAKE_OSX_SYSROOT="$sdk" -DLIBXML2_INCLUDE_DIR="$sdk/usr/include/libxml2" \
  -DZLIB_INCLUDE_DIR="$sdk/usr/include" -DOPENSSL_ROOT_DIR="$ssl_prefix" -DWITH_SSL="$ssl_prefix" \
  -DWITH_UNIT_TESTS=OFF -DPLUGIN_CHIMERA_MONGO=DYNAMIC \
  -DPLUGIN_ROCKSDB=NO -DPLUGIN_MROONGA=NO -DPLUGIN_SPIDER=NO \
  -DPLUGIN_OQGRAPH=NO -DPLUGIN_SPHINX=NO -DPLUGIN_CONNECT=NO \
  -DPLUGIN_COLUMNSTORE=NO -DPLUGIN_TOKUDB=NO
cmake --build "$build_dir/server" --target chimera_mongo
cmake -S "$HERE/probe" -B "$build_dir/wire-smoke" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir/wire-smoke"
cmake -S "$CHIMERA/cli/health" -B "$build_dir/health" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir/health"
install -m 755 "$build_dir/server/plugin/chimera_mongo/chimera_mongo.so" "$prefix/lib/chimeradb/plugin/"
install -m 755 "$build_dir/wire-smoke/chimeradb-wire-smoke" "$prefix/libexec/"
install -m 755 "$build_dir/health/chimeradb-health" "$prefix/libexec/"
install -m 755 "$CHIMERA/cli/chimeradb" "$prefix/libexec/chimeradb"
install -m 755 "$HERE/service.sh" "$prefix/libexec/chimeradb-service"
install -m 644 "$CHIMERA/sql/catalog.sql" "$prefix/share/chimeradb/sql/"
printf '%s\n' "$source_version" > "$prefix/share/chimeradb/mariadb-version"
printf '%s\n' "$resolved_server_prefix" > "$prefix/share/chimeradb/mariadb-prefix"
