#!/usr/bin/env bash
# Ground rule 1 (SSPL hygiene): the mongodb/ tree is a black-box reference and
# client binary. No ChimeraDB source may include, link, or vendor anything from
# it. This gate keeps that provable rather than aspirational.
#
#   check-hygiene.sh

source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

(($# == 0)) || die "check-hygiene.sh takes no arguments"

# Source and build inputs only — the test scripts legitimately *invoke* the
# reference binaries by path, which is the one permitted kind of dependency.
find_sources() {
  find "$CHIMERA_DIR" \
    -path "$CHIMERA_DIR/translator/build" -prune -o \
    -path "$CHIMERA_DIR/.run" -prune -o \
    -type f \( -name '*.c' -o -name '*.cc' -o -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \
               -o -name 'CMakeLists.txt' -o -name '*.cmake' \) -print0
}

count=$(find_sources | tr -dc '\0' | wc -c | tr -d ' ')
note "scanning $count source files for MongoDB tree references"

hits=$(find_sources | xargs -0 grep -nE 'mongodb/src|mongo/(base|bson|db|util)/|third_party/mongo' || true)
if [[ -n "$hits" ]]; then
  printf '%s\n' "$hits" >&2
  die "ChimeraDB source references the MongoDB tree — see ground rule 1"
fi

note "hygiene gate passed — no MongoDB source references"
