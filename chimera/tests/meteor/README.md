# Meteor acceptance test

The bar ChimeraDB is built to clear: a stock Meteor application, unmodified,
running reactively against MariaDB.

Nothing in here is an application we wrote. [run-meteor.sh](run-meteor.sh)
scaffolds `meteor create --full` into `chimera/.run/meteor` (untracked — it is
generated and large) and starts it with `MONGO_URL` and `MONGO_OPLOG_URL`
pointed at the chimera listener. The second variable is the interesting one: it
is what makes Meteor use its oplog observe driver instead of poll-and-diff, so
reactivity is being served by [the M5 oplog](../../plugin/chimera_mongo/oplog.cc)
rather than by Meteor re-running queries on a timer.

[probe-meteor.sh](probe-meteor.sh) is the fast inner loop. It issues the
handshake and bootstrap commands a Meteor server sends before it will serve a
page, and reports which ones chimera does not yet answer — a gap list to work
through without waiting on a Node build each time.

| Script | Purpose |
| --- | --- |
| `probe-meteor.sh --server <v>` | List unimplemented commands on the Meteor startup path |
| `run-meteor.sh --server <v> [--reset]` | Scaffold (once) and run the app on http://localhost:3000 |

The manual half of the milestone, which no script can assert: open two browsers,
add a todo in one and watch it appear in the other; then `INSERT` a todo with
the `mariadb` client and watch it appear in both.
