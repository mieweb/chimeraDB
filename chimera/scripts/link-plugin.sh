#!/usr/bin/env bash
# Symlinks the plugin source directory into both server trees. The server's own
# CMake auto-discovers subdirectories of plugin/, so this symlink is the ONLY
# thing that ever touches mariadb-server/ or mariadb-10.11/ (ground rule 2).
# It is regenerable: delete the links and re-run.
#
#   link-plugin.sh [--unlink]

source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

unlink_only=false
while (($#)); do
  case "$1" in
    --unlink) unlink_only=true; shift ;;
    *) die "unknown argument '$1'" ;;
  esac
done

PLUGIN_SRC="$CHIMERA_DIR/plugin/chimera_mongo"
[[ -d $PLUGIN_SRC ]] || die "no plugin source at $PLUGIN_SRC"

for tree in "$REPO_ROOT/mariadb-10.11" "$REPO_ROOT/mariadb-server"; do
  link="$tree/plugin/chimera_mongo"
  if $unlink_only; then
    [[ -L $link ]] && rm -f "$link" && note "removed $link"
    continue
  fi
  # Refuse to clobber a real directory — that would mean editing a server tree.
  if [[ -e $link && ! -L $link ]]; then
    die "$link exists and is not a symlink; refusing to touch the server tree"
  fi
  ln -sfn "$PLUGIN_SRC" "$link"
  note "linked $link -> $PLUGIN_SRC"
done
