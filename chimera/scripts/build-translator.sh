#!/usr/bin/env bash
# Build the translator and run its unit tests. Server-independent — the
# translator has no MariaDB dependency, which is exactly why it is a separate
# library, so this script takes no --server flag.
#
#   build-translator.sh [--clean]

source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

clean=false
while (($#)); do
  case "$1" in
    --clean) clean=true; shift ;;
    *) die "unknown argument '$1'" ;;
  esac
done

BUILD_DIR="$CHIMERA_DIR/translator/build"
$clean && rm -rf "$BUILD_DIR"

chimera_export_pkg_config_path

note "configuring translator"
cmake -S "$CHIMERA_DIR/translator" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo

note "building translator"
cmake --build "$BUILD_DIR"

note "running unit tests"
ctest --test-dir "$BUILD_DIR" --output-on-failure
