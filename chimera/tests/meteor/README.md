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
| `run-meteor.sh --server <v> [--release <x.y.z>] [--reactivity <order>] [--reset]` | Scaffold (once) and run the app on http://localhost:3000 |

## Which release, and which driver

Which reactivity driver Meteor picks is a property of the *release*, so the
release is pinned and both generations are kept scaffolded side by side under
`chimera/.run/meteor/todos-<release>`:

| Release | Default driver | Command |
| --- | --- | --- |
| 3.3.1 | oplog tailing | `run-meteor.sh --server <v>` |
| 3.5.1 | change streams | `run-meteor.sh --server <v> --release 3.5.1` |

Meteor 3.5 tries `changeStreams → oplog → polling` and picks the first driver
the server appears to offer, deciding once at driver-selection time. ChimeraDB
serves both, so either release runs on default settings and no environment
variable is needed.

Forcing the older driver is still useful as a regression check — it is how the
3.5 app is made to exercise the same path 3.4 and earlier take:

```sh
run-meteor.sh --server 11.8 --release 3.5.1 --reactivity oplog,polling
```

Which driver a run actually used is visible in the server's own query log rather
than anything the app reports: the change-stream path reads rendered events out
of `chimera_meta.oplog`, the oplog path reads the raw rows.

The manual half of the milestone, which no script can assert: open two browsers,
add a todo in one and watch it appear in the other; then `INSERT` a todo with
the `mariadb` client and watch it appear in both.
