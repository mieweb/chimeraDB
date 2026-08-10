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

## Loading the Mongo head

`run-server.sh` does this automatically once `build-plugin.sh --server <v>` has installed
`chimera_mongo.so` into the server's `lib/plugin/`, but the flags are worth knowing:

```sh
mariadbd --plugin-maturity=experimental \
         --plugin-load-add=chimera_mongo \
         --chimera-mongo-bind=127.0.0.1 \
         --chimera-mongo-port=27018
```

`--plugin-maturity=experimental` is **not optional**. The plugin honestly declares itself
`MariaDB_PLUGIN_MATURITY_EXPERIMENTAL`, and mariadbd's default floor is `gamma`, so without
the flag the library is refused — and because the refusal happens before its system
variables register, `--chimera-mongo-port` then reads as an unknown variable and the server
aborts startup entirely. A confusing cascade from one missing flag.

`chimera_mongo_bind` defaults to `127.0.0.1`, and every listener variable is
`PLUGIN_VAR_READONLY` — a listener cannot be moved out from under live connections. There is
no authentication on the Mongo port yet
([#5](https://github.com/mieweb/chimeraDB/issues/5)), so loopback is **enforced**, not merely
defaulted: a non-loopback `chimera_mongo_bind` refuses to start unless
`chimera_mongo_insecure_bind=ON` is also set, the opt-in is logged loudly at startup, and
`chimeraSql`/`$sql` are refused entirely on such a bind — an unauthenticated socket that can
reach the SQL gateway can read the whole server, not just collection tables.

Two build facts follow from living inside the server tree. MariaDB 10.11 compiles its whole
tree as C++11, so the plugin target sets `CXX_STANDARD 17` on itself to link the translator.
And `my_global.h` defines a macro named `array_elements`, which will silently mangle any C++
symbol of that name — the reason the translator's equivalent is called `array_values`, and a
good reason to keep `chimera_mongo.cc` the only file in the plugin that includes MariaDB
headers.

---

## The storage model, exactly

The [root README](../README.md#what-it-is) explains *why* a collection is a table. This is
*what* that means in DDL — the convention every later milestone writes against.

A Mongo database is a MariaDB database; a collection is a table:

```sql
CREATE TABLE <db>.<coll> (
  _id VARBINARY(255) NOT NULL PRIMARY KEY,   -- canonical id bytes from the translator (M2.3)
  doc JSON NOT NULL                          -- the whole document, canonical Extended JSON (D5)
) ENGINE=InnoDB;
```

`_id` is `VARBINARY` because Mongo ids are not all strings — ObjectIds are 12 raw bytes,
and binary comparison gives one deterministic ordering for every id type. MariaDB's `JSON`
is an alias for `LONGTEXT` plus an automatic `JSON_VALID` check constraint, so `doc` cannot
become invalid JSON no matter which head writes it. Only the catalog
([sql/catalog.sql](sql/catalog.sql)) records that the table is a collection; everything else
about it is already in `information_schema`.

The key bytes are a one-byte type tag followed by the id itself
([translator/src/id.cpp](translator/src/id.cpp)):

| Tag | Id type | Encoding |
|---|---|---|
| `0x07` | ObjectId | the 12 raw bytes, unchanged |
| `0x02` | string | UTF-8 bytes (Meteor's default random ids) |
| `0x12` | integer | 8 bytes, big-endian, sign bit flipped |

Big-endian with a flipped sign bit makes byte order *be* numeric order, so the InnoDB
primary key sorts integer ids correctly. `int32` and `int64` share one tag deliberately:
`1` and `NumberLong(1)` must be the same key, or a collection could hold two documents
Mongo considers duplicates. The cost is that an integral id decodes back as `int64`.

Extended JSON's type wrappers are just more JSON, so their `$`-prefixed keys are reachable
with quoted path members — a canonical date lives at
`$.createdAt."$date"."$numberLong"`.

Projections come in two directions (D3/D10):

| Direction | DDL | Guarantee |
|---|---|---|
| `forward` (default) | `ADD COLUMN c … AS (JSON_VALUE(doc,'$.path')) PERSISTENT \| VIRTUAL` | Engine-enforced. Direct writes are rejected (`ERROR 1906`), so the column physically cannot drift |
| `bidirectional` | real column + `BEFORE INSERT`/`BEFORE UPDATE` triggers | `UPDATE t SET c = …` is written through into `doc`; if one statement changes both, the document wins |

Two behaviors worth knowing before you design a projection, both demonstrated by
[scripts/demo-m1.sh](scripts/demo-m1.sh):

- **`PERSISTENT` cannot be added with `ALGORITHM=INSTANT`** (`ERROR 1845`). That refusal is
  the feature: the table rebuild is what materializes the column for every existing
  document. `VIRTUAL` columns *are* instant, because nothing is stored.
- **Type mismatches follow `sql_mode`, not a ChimeraDB policy.** Under MariaDB's default
  strict mode the `ALTER` fails outright; under a permissive `sql_mode` you get warning
  1366 and MariaDB's ordinary coercion.

---

## Speaking the other language

Each client can reach the other's dialect without leaving its own connection. Both routes
are demonstrated end to end by [scripts/demo-gateways.sh](scripts/demo-gateways.sh).

### SQL from a mongo client

`chimeraSql` runs a statement and returns its rows as documents, one document per row, in an
ordinary cursor — so a driver needs no special handling for it:

```js
db.runCommand({chimeraSql: "SELECT sku, qty FROM shop.parts WHERE qty < 5"})
// { cursor: { firstBatch: [ { sku: "bolt", qty: 3 } ], id: 0, ns: "shop.$cmd.chimeraSql" }, ok: 1 }
```

The same statement can be the *source* of an aggregation, which is the useful form: SQL does
the join, the pipeline does the reshaping.

```js
db.aggregate([
  {$sql: "SELECT o.id, c.name FROM orders o JOIN customers c ON c.id = o.customer_id"},
  {$sort: {name: 1}},
  {$limit: 10}
])
```

`{$sql: …}` must be the first stage — it produces the documents, so there is nothing for it
to read if a stage ran before it — and `$match` is not available after it. Filtering belongs
in the `WHERE` clause, which is the reason to reach for the stage at all.

Numbers come back as numbers and `NULL` comes back as `null`. `DECIMAL` is the deliberate
exception: it arrives as a string, because turning it into a double would throw away the
precision it was chosen for.

**Writes are refused by default.** The Mongo port has no authentication yet, so a mongo
client is only allowed to read:

```js
db.runCommand({chimeraSql: "DELETE FROM shop.parts"})
// { ok: 0, code: 13, codeName: "Unauthorized",
//   errmsg: "the SQL gateway is read-only; 'DELETE' needs SET GLOBAL chimera_mongo_sql_writes = ON" }
```

A DBA opens the door from the SQL side with `SET GLOBAL chimera_mongo_sql_writes = ON`. The
refusal is enforced twice: a keyword whitelist, and the server's own
`START TRANSACTION READ ONLY`. Neither alone is enough — a read-only transaction does not
stop DDL, because `CREATE`/`DROP` commit implicitly before they run, and a keyword check
cannot know what a view or a trigger does.

### mongosh from a SQL client

`mongo()` takes a statement copied straight out of a shell session:

```sql
CREATE FUNCTION mongo RETURNS STRING SONAME 'chimera_mongo.so';   -- once per datadir

USE shop;
SELECT mongo("db.parts.findOne({sku: 'bolt'})");
SELECT mongo("db.parts.updateOne({sku: 'bolt'}, {$set: {qty: 11}})");
SELECT mongo('shop', 'db.parts.countDocuments({})');              -- explicit database
```

The one-argument form uses the database you are already `USE`-ing; the two-argument form
names it. Supported verbs: `find`, `findOne`, `insertOne`, `insertMany`, `updateOne`,
`updateMany`, `replaceOne`, `deleteOne`, `deleteMany`, `countDocuments`, `aggregate`.
The second argument of `find`/`findOne` is a projection; sorting and paging are
`aggregate`'s job here, because a chained `.sort()` is a method call and this is a gateway,
not a JavaScript engine.

The result is Extended JSON: an array for `find`/`aggregate` (the whole result — the cursor
is drained, because a SQL caller has no way to ask for the rest), a document or `null` for
`findOne`, a plain number for `countDocuments`, and the command's own reply for a write. A
failed write raises a SQL error rather than returning a document that says it failed.

Every verb is turned into the same command document a driver would have sent and dispatched
through the same handlers, so a write from here takes the same locks and produces the same
single oplog entry. The gateway adds a spelling, not a second implementation.

**Quoting.** Arguments accept either quote style, which is what makes both of these work:

| `sql_mode` | Write it as |
|---|---|
| default | `mongo("db.parts.find({sku: 'bolt'})")` |
| `ANSI_QUOTES` | `mongo('db.parts.find({sku: "bolt"})')` |

Under `ANSI_QUOTES` the double-quoted outer form is an *identifier* to the SQL parser and
fails before `mongo()` ever runs. That is the server's rule, not ChimeraDB's, and it fails
loudly rather than half-working. Naked, unquoted mongo syntax at the SQL prompt would
require forking the server's parser, which ground rule 2 forbids — the string wrapper is the
supported form.

---

Milestone plan and the decisions behind all of this: [chimeraDB-plan.md](../chimeraDB-plan.md).
