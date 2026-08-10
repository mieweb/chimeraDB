# `chimera/` — everything ChimeraDB actually is

This directory is the whole product. Everything outside it (`mariadb-server/`,
`mariadb-10.11/`, `mongodb/`) is unmodified third-party source: two host servers we build
against and one black-box test oracle. **Nothing in those trees is ever edited** — the only
link between them and this directory is a regenerable symlink created by
[scripts/link-plugin.sh](scripts/link-plugin.sh).

The organizing principle: *one subdirectory per layer of the architecture diagram in the
[root README](../README.md#what-it-is)*, plus scripts and tests that cut across all of them.

| Directory | What anchors it |
|---|---|
| [plugin/chimera_mongo/](plugin/chimera_mongo/) | The Mongo head |
| [translator/](translator/) | The brain, shared by both heads |
| [sql/](sql/) | The storage conventions |
| [scripts/](scripts/) | Every action, replayable |
| [tests/](tests/) | Proof, at three levels |

## `plugin/chimera_mongo/`

The MongoDB wire-protocol listener, built as a MariaDB **daemon plugin** — it runs inside
`mariadbd`, in-process, so a document write and its projected columns and its oplog row all
commit in one InnoDB transaction. This is the only code that knows about sockets, OP_MSG
framing, cursors, and command dispatch; it delegates every question of *meaning* to the
translator. It is symlinked into each server tree's `plugin/` directory at build time so
the server's own CMake picks it up, which is why it never needs an upstream patch.

## `translator/`

A standalone C++ library with no MariaDB dependency, so it can be unit-tested without a
server. It owns the four conversions ChimeraDB lives or dies by: BSON ⇄ canonical Extended
JSON (the on-disk encoding), `_id` ⇄ the `VARBINARY` primary key, Mongo filter documents ⇒
parameterized SQL `WHERE` clauses, and update operators ⇒ a rewritten document. Both heads
use it — the wire plugin and the `mongo('…')` SQL gateway — which is what makes the two
languages provably equivalent instead of merely similar.

## `sql/`

The relational conventions expressed as SQL you can read: the `chimera_meta` catalog, the
shape of a collection table, the oplog table, the trigger templates that capture raw-SQL
writes, and the stored procedures a DBA calls to add a projection. Keeping these as plain
`.sql` files rather than strings inside C++ means a DBA can inspect, diff, and apply them
by hand — the storage model is documentation, not an implementation detail.

## `scripts/`

The executable definition of every build, run, and test action, so CI and humans invoke the
exact same thing (ground rule 3). Every script takes `--server 10.11|11.8` because every
claim must hold on both LTS lines (ground rule 4). Runtime state — datadirs, pidfiles, logs
— lands in `chimera/.run/`, which is gitignored.

## `tests/`

Three levels, deliberately separate. `unit/` tests the translator with no server running.
`differential/` runs identical operations against ChimeraDB and against the real `mongod`
oracle and diffs the results — the oracle decides what "MongoDB-compatible" means, so we
never have to guess. `meteor/` is the acceptance bar: a stock Meteor todos app that must run
reactively, oplog tailing and all.

---

Milestone plan and the decisions behind all of this: [chimeraDB-plan.md](../chimeraDB-plan.md).
