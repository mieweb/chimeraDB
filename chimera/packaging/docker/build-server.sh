#!/usr/bin/env bash
# Configure, build and install one MariaDB source tree into $SERVER_DIST.
#
#   build-server.sh --server 10.11|11.8 [--jobs N]
#
# On macOS this was done once by hand and build-plan.md owns that story. A fresh
# container has no such tree, so the same steps have to be a script. Storage
# engines ChimeraDB never loads are switched off: they are most of the build
# time and none of the coverage.

source "$(dirname "${BASH_SOURCE[0]}")/../../scripts/_common.sh"

chimera_parse_server "$@"
set -- ${CHIMERA_ARGS[@]+"${CHIMERA_ARGS[@]}"}

jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
while (($#)); do
  case "$1" in
    --jobs) jobs="${2:-}"; shift 2 ;;
    --jobs=*) jobs="${1#*=}"; shift ;;
    *) die "unknown argument '$1'" ;;
  esac
done

[[ -f $SERVER_TREE/CMakeLists.txt ]] || die "no MariaDB source tree at $SERVER_TREE"
chimera_export_pkg_config_path

note "configuring $SERVER_VERSION -> $SERVER_BUILD"
cmake -S "$SERVER_TREE" -B "$SERVER_BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_INSTALL_PREFIX="$SERVER_DIST" \
  -DWITH_UNIT_TESTS=OFF \
  -DPLUGIN_ROCKSDB=NO -DPLUGIN_MROONGA=NO -DPLUGIN_SPIDER=NO \
  -DPLUGIN_OQGRAPH=NO -DPLUGIN_SPHINX=NO -DPLUGIN_CONNECT=NO \
  -DPLUGIN_COLUMNSTORE=NO -DPLUGIN_TOKUDB=NO

note "building $SERVER_VERSION with $jobs jobs (this is the long one)"
cmake --build "$SERVER_BUILD" -j "$jobs"

note "installing $SERVER_VERSION -> $SERVER_DIST"
cmake --install "$SERVER_BUILD" >/dev/null

chimera_require_dist
note "$("$MARIADBD" --version)"
