#!/usr/bin/env bash
# Builds the chimera_mongo daemon plugin against one server version and installs
# it into that server's dist/ plugin directory.
#
#   build-plugin.sh --server 10.11|11.8

source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

chimera_parse_server "$@"
((${#CHIMERA_ARGS[@]} == 0)) || die "unknown argument '${CHIMERA_ARGS[0]}'"

# The plugin links the translator static library, so it has to exist first.
"$CHIMERA_DIR/scripts/build-translator.sh" >/dev/null
"$CHIMERA_DIR/scripts/link-plugin.sh"

[[ -d $SERVER_BUILD ]] || die "no build directory at $SERVER_BUILD"
export PKG_CONFIG_PATH="$(brew --prefix)/lib/pkgconfig:${PKG_CONFIG_PATH:-}"

note "re-running cmake for $SERVER_VERSION so it picks up the new plugin directory"
cmake "$SERVER_BUILD" >/dev/null

note "building chimera_mongo for $SERVER_VERSION"
cmake --build "$SERVER_BUILD" --target chimera_mongo

PLUGIN_LIB=$(find "$SERVER_BUILD" -name 'chimera_mongo.so' -o -name 'chimera_mongo.dylib' | head -1)
[[ -n $PLUGIN_LIB ]] || die "cmake reported success but no chimera_mongo module was produced"

install -m 755 "$PLUGIN_LIB" "$SERVER_DIST/lib/plugin/"
note "installed $(basename "$PLUGIN_LIB") into $SERVER_DIST/lib/plugin/"
