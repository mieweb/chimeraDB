#!/usr/bin/env bash
# Build the package health helper independently of either MariaDB source tree.
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"
build_dir="${CHIMERA_OUT:-$CHIMERA_DIR/.run}/health"
testing=OFF
while (($#)); do
  case $1 in
    --build-dir) build_dir=${2:?missing build directory}; shift 2 ;;
    --test) testing=ON; shift ;;
    *) die "usage: build-health.sh [--build-dir DIR] [--test]" ;;
  esac
done
chimera_export_pkg_config_path
cmake -S "$CHIMERA_DIR/cli/health" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING="$testing"
cmake --build "$build_dir"
if [[ $testing == ON ]]; then ctest --test-dir "$build_dir" --output-on-failure; fi
note "built $build_dir/chimeradb-health"
