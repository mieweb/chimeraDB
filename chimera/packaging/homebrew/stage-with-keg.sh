#!/usr/bin/env bash
# Build and test against an exact installed Homebrew MariaDB keg. This tests
# package staging when Homebrew itself cannot build formulae on the host.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
die() { printf 'homebrew staging: %s\n' "$*" >&2; exit 1; }
server= work=
while (($#)); do
  case "$1" in
    --server) server=${2:?}; shift 2 ;;
    --work-dir) work=${2:?}; shift 2 ;;
    *) die "unknown argument: $1" ;;
  esac
done
[[ $server == 10.11 || $server == 11.8 ]] || die '--server 10.11|11.8 is required'
work=${work:-$REPO/chimera/.run/release/homebrew-stage-$server}
mkdir -p "$work"
work=$(cd "$work" && pwd)
export HOMEBREW_NO_AUTO_UPDATE=1 HOMEBREW_NO_INSTALL_CLEANUP=1
server_prefix=$(brew --prefix "mariadb@$server")
[[ -x $server_prefix/bin/mariadbd ]] || die "install mariadb@$server first"

# brew fetch verifies the source checksum and reuses the cache shared with the
# formula resource. It neither builds MariaDB nor modifies its installed keg.
brew fetch --build-from-source "mariadb@$server"
archive=$(brew --cache --build-from-source "mariadb@$server")
[[ -f $archive ]] || die "source archive missing: $archive"
archive_sha=$(shasum -a 256 "$archive")
archive_sha=${archive_sha%% *}
identity="$work/source/.chimera-archive-sha256"
if [[ ! -f $work/source/VERSION || ! -f $identity || $(cat "$identity") != "$archive_sha" ]]; then
  # Validate a complete replacement before discarding a previous usable cache.
  # VERSION alone is insufficient: a newer patch keg changes both its source
  # archive and the server CMake cache, even when --work-dir stays the same.
  source_tmp=$(mktemp -d "$work/.source.XXXXXX")
  cleanup_source() { [[ -z $source_tmp ]] || rm -rf "$source_tmp"; }
  trap cleanup_source EXIT
  tar -xzf "$archive" -C "$source_tmp" --strip-components=1
  [[ -f $source_tmp/VERSION ]] || die 'source archive has no MariaDB VERSION file'
  source_version=$(awk -F= '/^MYSQL_VERSION_(MAJOR|MINOR|PATCH)=/ {v=v sep $2; sep="."} END {print v}' "$source_tmp/VERSION")
  runtime_version=$("$server_prefix/bin/mariadbd" --version | sed -E 's/.*(Ver|Distrib) ([0-9]+\.[0-9]+\.[0-9]+).*/\2/')
  [[ $source_version == "$server".* && $source_version == "$runtime_version" ]] ||
    die "MariaDB source $source_version does not match runtime $runtime_version ($server required)"
  printf '%s\n' "$archive_sha" > "$source_tmp/.chimera-archive-sha256"
  rm -rf "$work/source" "$work/build"
  mv "$source_tmp" "$work/source"
  source_tmp=
fi
export CMAKE_BUILD_PARALLEL_LEVEL=${CMAKE_BUILD_PARALLEL_LEVEL:-$(sysctl -n hw.ncpu)}
"$HERE/build.sh" --server "$server" --mariadb-source "$work/source" \
  --mariadb-prefix "$server_prefix" --prefix "$work/prefix" --build-dir "$work/build"
# Keep the socket path well under macOS's sockaddr_un limit, even when the
# source checkout or TMPDIR has a long name. Test state is kept for inspection.
smoke_dir=$(mktemp -d "/tmp/chimeradb-keg-$server.XXXXXX")
printf 'Isolated staging test: %s\n' "$smoke_dir"
"$HERE/smoke.sh" --server "$server" --mariadb-prefix "$server_prefix" \
  --prefix "$work/prefix" --work-dir "$smoke_dir"
printf 'Staged prefix: %s\n' "$work/prefix"
