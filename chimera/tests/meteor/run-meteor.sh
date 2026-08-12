#!/usr/bin/env bash
# M6: a stock Meteor app run against ChimeraDB. This is the acceptance test —
# not a unit test we wrote to pass, but somebody else's application driving the
# wire protocol however it likes.
#
#   run-meteor.sh --server <v> [--release <x.y.z>] [--reactivity <order>] [--reset]
#
# The app itself is `meteor create --full`, scaffolded on first use into
# chimera/.run/meteor (untracked: it is generated, and it is large). Only the
# environment is ours, and it is the whole point:
#
#   MONGO_URL       — normal data access
#   MONGO_OPLOG_URL — makes Meteor use the oplog observe driver rather than
#                     poll-and-diff, which is what M5 exists to support
#
# --release pins the Meteor version, because which reactivity driver Meteor
# chooses is a property of the release: 3.4 and earlier tail the oplog, 3.5 and
# later prefer change streams. Both are kept scaffolded side by side so a change
# to one head cannot silently break the other. --reactivity overrides that
# choice (METEOR_REACTIVITY_ORDER), which is how a 3.5 app is forced back onto
# the oplog path.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../../scripts/_common.sh"
chimera_parse_server "$@"

RESET=0
RELEASE="3.3.1"
REACTIVITY=""
args=("${CHIMERA_ARGS[@]+"${CHIMERA_ARGS[@]}"}")
while ((${#args[@]} > 0)); do
  case "${args[0]}" in
    --reset) RESET=1; args=("${args[@]:1}") ;;
    --release) RELEASE="${args[1]:-}"; [[ -n $RELEASE ]] || die "--release needs a version"; args=("${args[@]:2}") ;;
    --reactivity) REACTIVITY="${args[1]:-}"; [[ -n $REACTIVITY ]] || die "--reactivity needs an order"; args=("${args[@]:2}") ;;
    *) die "unknown argument '${args[0]}'" ;;
  esac
done

APP_DIR="$RUN_DIR/meteor/todos-$RELEASE"
command -v meteor >/dev/null || die "meteor is not installed — see https://meteor.com/install"

((RESET == 0)) || rm -rf "$APP_DIR"

if [[ ! -d $APP_DIR ]]; then
  note "scaffolding the stock Meteor $RELEASE todos app into $APP_DIR"
  mkdir -p "$(dirname "$APP_DIR")"
  (cd "$(dirname "$APP_DIR")" &&
    meteor create --full --release "METEOR@$RELEASE" "$(basename "$APP_DIR")")
fi

# `meteor create` writes package.json but does not always install it, and the
# app will not boot without @babel/runtime. Cheap to repeat, fatal to skip.
(cd "$APP_DIR" && meteor npm install)

# The Meteor 3 scaffold still calls the synchronous `Links.insert`, which that
# release removed — a bug in the generated app, not in ChimeraDB, but it stops
# the demo at the first click. Patching it here keeps the fix reproducible
# instead of hand-edited into an untracked directory.
METHODS="$APP_DIR/imports/api/links/methods.js"
if [[ -f $METHODS ]] && grep -q 'return Links.insert(' "$METHODS"; then
  note "patching the scaffold's synchronous Links.insert (Meteor 3 removed it)"
  sed -i '' 's/return Links.insert(/return Links.insertAsync(/' "$METHODS"
fi

chimera_require_running

export MONGO_URL="mongodb://127.0.0.1:$MONGO_PORT/meteor"
export MONGO_OPLOG_URL="mongodb://127.0.0.1:$MONGO_PORT/local"

note "MONGO_URL=$MONGO_URL"
note "MONGO_OPLOG_URL=$MONGO_OPLOG_URL"
if [[ -n $REACTIVITY ]]; then
  export METEOR_REACTIVITY_ORDER="$REACTIVITY"
  note "METEOR_REACTIVITY_ORDER=$METEOR_REACTIVITY_ORDER"
fi
note "starting Meteor $RELEASE on http://localhost:3000 (Ctrl-C to stop)"

# --port 3000 explicitly: without it Meteor would also start a Mongo of its own
# on the next port, and there must be no second database anywhere in this
# picture. Setting MONGO_URL already suppresses that, but the pairing is easier
# to reason about when the port is stated.
cd "$APP_DIR"
exec meteor run --port 3000
