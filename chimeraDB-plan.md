# chimeraDB Implementation Plan

**Date:** 2026-08-10
**Workspace:** `/Volumes/Case/prj/chimeraSQL`
**Goal:** A MongoDB-compatible document store implemented *inside* MariaDB — single storage
engine (InnoDB), single transaction domain — good enough to run a Meteor.js app
(oplog tailing included), with documents auto-projected into relational columns.
**Server targets:** MariaDB **10.11 (LTS)** and **11.8 (LTS)** — every milestone's exit
criteria must pass on **both**.

> **Doc map (DRY):** [README.md](README.md) owns the *what & why* — product pitch,
> architecture diagram, compatibility surface, licensing/trademark statement.
> [build-plan.md](build-plan.md) owns the base-binary build recipes. This file owns the
> *how & when* — engineering decisions, milestones, exit criteria — and does not restate
> the other two. The `mongodb/` tree (r8.0.12) is used **only** as a test oracle and for
> its `mongo` shell client.

---

## Architecture

The architecture diagram, the anatomy of a collection table, and the product positioning
live in [README § What it is](README.md#what-it-is) — not restated here. The table below
maps each component in that diagram to where it gets built:

| Component (README diagram) | Source location | Built in |
|---|---|---|
| `chimera_mongo` daemon plugin — listener, commands, cursors | `chimera/plugin/chimera_mongo/` | M3 skeleton · M4 CRUD · M5 tailable cursors |
| Translator — BSON⇄extJSON codec, `_id` canonicalization, filter/update compiler | `chimera/translator/` | M2 |
| Doc-store conventions — collection tables, `chimera_meta` catalog | `chimera/sql/` | M1 |
| Projection manager — `manual` today; `eager`/`lazy` knobs | `chimera/sql/` + plugin (`createIndexes`) | M1, M4 (automation: M8) |
| Transactional oplog — same-txn append, triggers, pruning, tailable cursors | `chimera/sql/` + plugin | M5 |
| Cross-language gateways — `mongo_find()` UDF, `chimeraSql`/`$sql` | plugin + translator | M7 |
| Meteor acceptance harness | `chimera/tests/meteor/` | M6 |

### Locked decisions

| # | Decision | Rationale |
|---|---|---|
| D1 | One engine (InnoDB), two protocol heads | Only shape where doc+projection writes are atomic; mongod exposes no XA so dual-engine can never be transaction-safe |
| D2 | Compatibility bar = **Meteor.js** | Oplog tailing is the linchpin (hello presents a single-node replica set). The **authoritative** supported / not-supported command surface is the single table in [README § Compatibility](README.md#compatibility) — milestones implement exactly that list, nothing more |
| D3 | Document is **source of truth**; projections are `GENERATED ALWAYS` columns | Engine-enforced: projection columns cannot drift. `ALTER … ADD COLUMN … AS (JSON_VALUE(doc,'$.path')) PERSISTENT` backfills natively (the ALTER rebuild *is* the scan) |
| D4 | Projection mode per collection: `manual` (default) \| `eager` \| `lazy` | Manual = DBA issues ALTERs. Eager = auto-ALTER on new paths (later). Lazy = no physical columns, JSON_VALUE on demand |
| D5 | Storage encoding = **MongoDB Extended JSON (canonical)** in a `JSON` column | MariaDB JSON is text; extJSON preserves BSON types (ObjectId, Date, Timestamp, Decimal128). libbson converts both directions for free |
| D6 | Update execution = **read-modify-write** (doc locked `FOR UPDATE`, ops applied via libbson in memory, full doc written back) | KISS: correct for *all* update operators under InnoDB row locking. Compile-to-SQL is a later optimization, not a requirement |
| D7 | Oplog = InnoDB table written **in the same transaction** as the mutation; triggers on collection tables catch raw-SQL writes | Transactional oplog (never shows rolled-back writes) — stronger than real MongoDB. In-process commit notification wakes tailing cursors |
| D8 | Type-mismatch policy: permissive (`JSON_VALUE` → NULL + warning) by default; strict mode later | Matches MariaDB semantics; documented knob |
| D9 | All chimera code is GPLv2, out-of-tree, in `chimera/` | Zero patches to either upstream. Outbound licensing & trademark statement: [README § License & trademarks](README.md#license--trademarks) |

### Non-negotiable ground rules

1. **SSPL hygiene:** never copy, port, or paraphrase code from `mongodb/` into `chimera/`.
   The mongodb tree is a *black-box* test oracle and client binary only. Implement the wire
   protocol from public documentation and observed behavior (FerretDB proved cleanroom
   viability). A CI check greps `chimera/` for `mongodb/src` includes — it must stay empty.
2. **Zero upstream patches:** nothing inside `mariadb-server*/` trees may be edited except
   a regenerable symlink into `plugin/`. If a server patch ever seems necessary, **stop and
   escalate** — that changes the maintainability story.
3. **Script-first:** every build/run/test action is a script in `chimera/scripts/` that CI
   and humans invoke identically.
4. **Dual-version always:** every script takes `--server 10.11|11.8`; every milestone exit
   criterion is checked against both builds.
5. **Commit per checkbox-group** with message prefix `M<n>:` and keep the checkboxes in
   this file updated in the same commit (same workflow as [build-plan.md](build-plan.md)).

---

## Repository layout (target state)

```
chimeraSQL/
├── README.md                   # product front page: what & why, quick start, compatibility
├── build-plan.md               # how the base binaries were built (done)
├── chimeraDB-plan.md           # this file: how & when
├── mariadb-server/             # 11.8.8 tree + build/ (gitignored)
├── mariadb-server-10.11/       # 10.11.x tree + build/ (gitignored)
├── mongodb/                    # r8.0.12 — ORACLE + mongo shell ONLY (gitignored)
└── chimera/
    ├── README.md               # folder anchor: what lives here and why
    ├── plugin/chimera_mongo/   # daemon plugin source (symlinked into each server tree)
    ├── translator/             # shared C++ lib: extJSON codec, filter/update compiler
    ├── sql/                    # catalog DDL, stored procedures, trigger templates
    ├── scripts/                # build-plugin.sh, run-server.sh, stop-server.sh, test-*.sh
    └── tests/
        ├── unit/               # translator unit tests (ctest)
        ├── differential/       # same ops vs real mongod, diff results
        └── meteor/             # end-to-end Meteor app harness
```

Dev port conventions (so everything can run simultaneously):

| Service | Port |
|---|---|
| chimera on 11.8 (MySQL proto) | 3307 |
| chimera on 10.11 (MySQL proto) | 3308 |
| chimera_mongo listener (11.8) | 27018 |
| chimera_mongo listener (10.11) | 27019 |
| oracle `mongod` | 27117 |

---

## Milestone 0 — Dual-version scaffolding

**Goal:** both MariaDB versions build and run via scripts; `chimera/` skeleton exists.

- [ ] **M0.1** Create the `chimera/` directory skeleton above with a `README.md` explaining
  the folder's organizing principle (one paragraph per subdirectory).
- [ ] **M0.2** Add `mariadb-server-10.11/` to [.gitignore](.gitignore) (same pattern as the
  existing trees).
- [ ] **M0.3** Normalize the 10.11 checkout into its own tree so both versions coexist
  (skip if already done — a 10.11 build is currently in progress):
  ```sh
  cd mariadb-server && git worktree add ../mariadb-server-10.11 mariadb-10.11
  ```
  Configure/build with the *same recipe and SDK lessons* as
  [build-plan.md](build-plan.md) Track A (explicit `SDKROOT`, brew bison first in PATH,
  `-DPLUGIN_COLUMNSTORE=NO -DPLUGIN_ROCKSDB=NO`).
- [ ] **M0.4** Verify both trees report the right versions:
  ```sh
  mariadb-server/build/sql/mariadbd --version         # 11.8.8
  mariadb-server-10.11/build/sql/mariadbd --version   # 10.11.x
  ```
- [ ] **M0.5** Create an installed layout for each tree (needed for `mariadb-install-db`,
  plugin dirs, and mtr later — this is the gap that blocked the earlier smoke test):
  ```sh
  cmake --install mariadb-server/build --prefix "$PWD/mariadb-server/dist"
  cmake --install mariadb-server-10.11/build --prefix "$PWD/mariadb-server-10.11/dist"
  ```
- [ ] **M0.6** Write `chimera/scripts/run-server.sh --server 10.11|11.8 [--fresh]` and
  `stop-server.sh`: init datadir with `mariadb-install-db` on first run, start `mariadbd`
  with the port conventions above, pidfile under `chimera/.run/`. Never hardcode paths —
  derive from `--server`.
- [ ] **M0.7** Install the Apache-2.0 BSON/Mongo C libraries used by the translator:
  ```sh
  brew install mongo-c-driver
  pkg-config --list-all | grep -iE 'bson|mongoc'   # note the pkg names (1.x: libbson-1.0; 2.x: bson2)
  ```
- [ ] **M0.8** JSON feature probe on both servers (via `mariadb` client against each):
  `SELECT JSON_VALUE('{"a":{"b":2}}','$.a.b');` returns `2`;
  `CREATE TEMPORARY TABLE t (d JSON, v INT AS (JSON_VALUE(d,'$.x')) VIRTUAL);` succeeds.
- [ ] **M0.9** Verify the plugin precedents exist in **both** trees (they anchor M3):
  `plugin/daemon_example/`, `plugin/handler_socket/`, `plugin/test_sql_service/`.

**Exit criteria:**
- [ ] `run-server.sh --server 11.8` and `--server 10.11` start clean servers; probe SQL passes on both; skeleton committed.

---

## Milestone 0.5 — CONNECT↔mongod sandbox *(optional, timeboxed)*

**Goal:** zero-code preview of "SQL over live Mongo documents" to inform projection
ergonomics, and a future data-migration bridge. Skippable without affecting later milestones.

> Caveat: the CONNECT engine's MONGO table type requires **libmongoc-1.0** (driver 1.x)
> at configure time ([storage/connect/CMakeLists.txt#L341](mariadb-server/storage/connect/CMakeLists.txt#L341)).
> If brew installed driver 2.x in M0.7, either install a 1.x driver alongside or skip this milestone.

- [ ] **M0.5.1** Reconfigure the 11.8 tree so CMake reports `CONNECT_MONGODB: ON`; rebuild the CONNECT plugin only.
- [ ] **M0.5.2** Start the oracle mongod (port 27117) and seed a few documents via the built `mongo` shell.
- [ ] **M0.5.3** In the 11.8 server: `INSTALL SONAME 'ha_connect';` then
  `CREATE TABLE users ENGINE=CONNECT TABLE_TYPE=MONGO TABNAME='test.users' CONNECTION='mongodb://127.0.0.1:27117';`
  (no column list → exercises **discovery**: CONNECT samples documents and infers columns).
- [ ] **M0.5.4** Record findings in `chimera/README.md` (§ prior art): how nested paths flatten,
  how types map, what discovery gets wrong. These observations feed D4/D5 defaults.

**Exit criteria:**
- [ ] Findings paragraph committed (or milestone explicitly marked skipped here).

---

## Milestone 1 — Doc-store core, pure SQL (no C++ yet)

**Goal:** prove the storage model end-to-end with plain SQL on both versions.

- [ ] **M1.1** Write `chimera/sql/catalog.sql`:
  - `CREATE DATABASE IF NOT EXISTS chimera_meta;`
  - `chimera_meta.collections(db_name, coll_name, projection_mode ENUM('manual','eager','lazy') DEFAULT 'manual', created_at, PRIMARY KEY(db_name, coll_name))`
- [ ] **M1.2** Document the collection table convention in `chimera/README.md`:
  - Mongo database ⇒ MariaDB database; collection ⇒ table.
  - `CREATE TABLE <db>.<coll> (_id VARBINARY(255) NOT NULL PRIMARY KEY, doc JSON NOT NULL) ENGINE=InnoDB;`
    (MariaDB's `JSON` alias adds the `JSON_VALID` check automatically.)
  - `_id` holds the canonical byte form produced by the translator (M2); for now use plain strings.
- [ ] **M1.3** Write `chimera/scripts/demo-m1.sh --server <v>` executing this walkthrough
  and asserting each result:
  - [ ] create a `test.users` collection table + catalog row
  - [ ] insert two extJSON documents (one with `{"$date": ...}` field)
  - [ ] update one via `UPDATE … SET doc = JSON_SET(doc, '$.age', 31)`
  - [ ] `ALTER TABLE test.users ADD COLUMN email VARCHAR(190) AS (JSON_VALUE(doc,'$.email')) PERSISTENT, ADD INDEX(email);`
        — then `SELECT email FROM test.users` proves the **backfill happened during the ALTER** (D3)
  - [ ] add a `VIRTUAL` + indexed column too; note no rebuild occurred
  - [ ] prove drift is impossible: `UPDATE test.users SET email='x'` → error (generated column)
  - [ ] type-mismatch probe (D8): declare an `INT` projection over a string path → NULL + warning captured
  - [ ] nested/typed path example: `JSON_VALUE(doc, '$.createdAt."$date"')` extracts the extJSON date
- [ ] **M1.4** Run the demo against **both** servers and fix any 10.11/11.8 divergence found
  (record divergences in `chimera/README.md`).

**Exit criteria:**
- [ ] `demo-m1.sh` green on 10.11 and 11.8.

---

## Milestone 2 — Translator library (standalone C++, no server)

**Goal:** the shared brain used later by both the wire plugin and the UDF gateway.
Lives in `chimera/translator/`, builds with its own CMake, tests with ctest.

- [ ] **M2.1** Scaffold `chimera/translator/` (CMake, pkg-config for libbson — support both
  1.x and 2.x pkg names) + a unit-test target (single-header framework such as doctest).
- [ ] **M2.2** **Codec module:** BSON ⇄ canonical Extended JSON using libbson's built-ins
  (`bson_as_canonical_extended_json` / `bson_new_from_json`). Round-trip tests covering
  ObjectId, Date, Timestamp, Decimal128, Binary, nested arrays.
- [ ] **M2.3** **`_id` canonicalization:** ObjectId | string | int → deterministic bytes for
  the `VARBINARY(255)` PK, and back. Covers Meteor random-string ids *and*
  `idGeneration:'MONGO'` ObjectIds. Property test: encode→decode is identity; ordering is stable.
- [ ] **M2.4** **Filter compiler** (Meteor/minimongo subset): implicit `$eq`, `$gt/$gte/$lt/$lte/$ne`,
  `$in/$nin`, `$and/$or/$not`, `$exists`, `$regex`, basic `$elemMatch` → parameterized SQL
  `WHERE` over `JSON_VALUE`/`JSON_EXTRACT`/`JSON_CONTAINS` on `doc`. Unsupported operator ⇒
  clean, specific error (fail fast; no silent wrong answers). Every generated fragment uses
  bind parameters — **no string interpolation of user values** (injection surface).
- [ ] **M2.5** **Update engine** (per D6): apply `$set/$unset/$inc/$push/$pull/$addToSet/$pop`
  (positional `$` deferred to backlog) to a BSON doc **in memory**; returns new doc + a
  changed-fields summary (used by the oplog writer in M5). Unit tests mirror MongoDB's
  documented semantics for each operator, including edge cases (missing paths, arrays).
- [ ] **M2.6** Hygiene gate: `chimera/scripts/check-hygiene.sh` — fails if anything under
  `chimera/` includes or references `mongodb/src` (rule 1). Wire into `test.sh`.

**Exit criteria:**
- [ ] `ctest` green; hygiene gate green. (Server-independent — no dual-version matrix here.)

---

## Milestone 3 — Daemon plugin skeleton (wire listener, handshake)

**Goal:** `mongo` shell connects to mariadbd and can `ping` — on both server versions.

- [ ] **M3.1** Read the two in-tree precedents before writing code:
  [plugin/daemon_example](mariadb-server/plugin/daemon_example) (minimal daemon plugin
  lifecycle) and [plugin/handler_socket](mariadb-server/plugin/handler_socket) (a plugin
  running its own network listeners). Note how `st_maria_plugin` is declared, and how
  init/deinit manage threads.
- [ ] **M3.2** Create `chimera/plugin/chimera_mongo/` with `CMakeLists.txt` using
  `MYSQL_ADD_PLUGIN(chimera_mongo … MODULE_ONLY)`; declare `PLUGIN_LICENSE_GPL` and
  `MariaDB_PLUGIN_MATURITY_EXPERIMENTAL`. Link the translator static lib + libbson.
- [ ] **M3.3** Write `chimera/scripts/link-plugin.sh` — symlinks the plugin dir into each
  server tree's `plugin/` (server CMake auto-discovers subdirectories) and re-runs cmake:
  ```sh
  ln -sfn "$PWD/chimera/plugin/chimera_mongo" mariadb-server/plugin/chimera_mongo
  ln -sfn "$PWD/chimera/plugin/chimera_mongo" mariadb-server-10.11/plugin/chimera_mongo
  ```
  This symlink is the **only** thing that ever touches the server trees (rule 2).
  Expect the plugin to need `#if MYSQL_VERSION_ID` guards for 10.11 vs 11.8 API drift —
  keep them few and commented.
- [ ] **M3.4** Plugin skeleton: system variables `chimera_mongo_port` (defaults per port
  table), `chimera_mongo_bind` (**default `127.0.0.1`** — no auth exists yet, never bind
  wide by default); listener thread started in plugin init, joined in deinit (server must
  shut down clean, no leaked threads).
- [ ] **M3.5** Wire framing: implement **OP_MSG**, *plus* legacy **OP_QUERY only for the
  initial `isMaster`/`hello` handshake* — drivers and the legacy shell send their first
  handshake as OP_QUERY before switching to OP_MSG. Reply with `OP_REPLY` for that one
  path. Everything else is OP_MSG-only.
- [ ] **M3.6** Commands: `hello`/`isMaster` (present as single-node replica set:
  `isWritablePrimary:true`, `setName:"chimera"`, `me`/`hosts`, `logicalSessionTimeoutMinutes:30`,
  `maxWireVersion:17`, `minWireVersion:0` — document why 17), `ping`, `buildInfo`,
  `endSessions` (accept + no-op), and a proper error envelope (`ok:0, code, codeName, errmsg`).
- [ ] **M3.7** Manual verification with the built oracle shell against **both** servers:
  ```sh
  mongodb/build/install/bin/mongo --port 27018 --quiet --eval 'db.runCommand({ping:1})'
  mongodb/build/install/bin/mongo --port 27019 --quiet --eval 'db.runCommand({ping:1})'
  ```
- [ ] **M3.8** `chimera/scripts/build-plugin.sh --server <v>` + extend `run-server.sh` to
  `INSTALL SONAME 'chimera_mongo'` (or `--plugin-load-add`) automatically.

**Exit criteria:**
- [ ] Shell connects, `ping` and `hello` return well-formed replies on 10.11 **and** 11.8; clean server shutdown.

---

## Milestone 4 — CRUD over the wire

**Goal:** the Meteor CRUD surface works, verified differentially against real mongod.

- [ ] **M4.1** Execute SQL from inside the plugin via the server's **SQL service** —
  study [plugin/test_sql_service](mariadb-server/plugin/test_sql_service) first; all
  chimera SQL runs through one internal helper (single choke point for txn control + binds).
- [ ] **M4.2** Commands, each with its own checkbox and differential spec file:
  - [ ] `create` (collection) → table DDL + catalog row (+ triggers placeholder for M5)
  - [ ] `insert` (ordered batches, duplicate-`_id` → code 11000 `DuplicateKey`)
  - [ ] `find` with filter/projection/sort/limit/skip/batchSize → cursor machinery
  - [ ] `getMore` / `killCursors` (cursor registry with timeouts)
  - [ ] `update` (multi, upsert; RMW per D6 inside one InnoDB txn per doc batch)
  - [ ] `delete` (single + multi)
  - [ ] `findAndModify`
  - [ ] `count`, `distinct`
  - [ ] `listDatabases`, `listCollections`, `listIndexes`
  - [ ] `createIndexes` → `ALTER TABLE … ADD COLUMN … AS (JSON_VALUE(doc,…)) VIRTUAL, ADD INDEX`
        honoring the collection's projection mode; `dropIndexes`
  - [ ] `drop`, `dropDatabase`
  - [ ] implicit sessions: accept & ignore `lsid`/`txnNumber` on all of the above
- [ ] **M4.3** Differential harness `chimera/tests/differential/run.sh --server <v>`:
  starts oracle mongod (27117) + chimera; runs each `.js` spec through the oracle `mongo`
  shell against **both** endpoints; normalizes (`$clusterTime`, `operationTime`, cursor ids,
  key order) and diffs. A spec passes only if outputs match.
- [ ] **M4.4** Error-parity specs: unknown collection, bad filter operator, duplicate key —
  same `code`/`codeName` as the oracle where Meteor depends on them.

**Exit criteria:**
- [ ] Differential suite green on 10.11 **and** 11.8.

---

## Milestone 5 — Oplog + tailable cursors (the Meteor enabler)

**Goal:** `local.oplog.rs` emulation with transactional guarantees (D7).

- [ ] **M5.1** Schema in `chimera/sql/oplog.sql`: `chimera_meta.oplog(seq BIGINT AUTO_INCREMENT PK, ts_t INT UNSIGNED, ts_i INT UNSIGNED, op ENUM('i','u','d'), ns VARCHAR(512), o JSON, o2 JSON NULL)`;
  Timestamp rule: `ts_t` = unix seconds, `ts_i` = per-second counter derived under the same lock as `seq`.
- [ ] **M5.2** Translator/plugin write path: every insert/update/delete appends its oplog row
  **in the same transaction** ('u' entries use full-document replacement style in `o`,
  `{_id}` in `o2` — simplest form Meteor accepts).
- [ ] **M5.3** SQL-side capture: `chimera/sql/triggers.tpl.sql` — AFTER INSERT/UPDATE/DELETE
  triggers per collection table appending equivalent oplog rows; installed by `create`
  (M4.2) and by a `chimera_adopt_table` procedure for pre-existing tables. Guard against
  double-write when the mutation came through the plugin (session variable flag).
- [ ] **M5.4** Capped-collection emulation: background pruning of `chimera_meta.oplog`
  by age/row-count (plugin timer thread; both knobs are system variables).
- [ ] **M5.5** Tailable + `awaitData` cursors on `local.oplog.rs`: map the namespace to the
  oplog table; support Meteor's exact query shapes (`ts: {$gt: <Timestamp>}`, ns filtering,
  initial "latest entry" fetch); `getMore` parks on an in-process condition variable
  signaled at commit (no polling), honoring `maxTimeMS`.
- [ ] **M5.6** Demo script `chimera/scripts/demo-oplog.sh --server <v>`: shell A tails the
  oplog; shell B inserts via the wire; a third write goes through the **`mariadb` SQL
  client** — all three appear on the tail, in commit order.
- [ ] **M5.7** Unit-test Meteor's oplog query shapes and Timestamp round-tripping.

**Exit criteria:**
- [ ] `demo-oplog.sh` shows wire-writes *and* raw-SQL writes streaming to a tailing cursor, on both versions.

---

## Milestone 6 — Meteor end-to-end (acceptance test)

**Goal:** a stock Meteor todos app runs reactively against chimera. This is the bar.

- [ ] **M6.1** `chimera/tests/meteor/`: install Meteor, scaffold the standard todos example,
  script `run-meteor.sh --server <v>` exporting:
  ```sh
  export MONGO_URL="mongodb://127.0.0.1:27018/meteor"
  export MONGO_OPLOG_URL="mongodb://127.0.0.1:27018/local"
  ```
- [ ] **M6.2** Fix the gap list until startup is clean (expected suspects: the aggregation
  subset promised in [README § Compatibility](README.md#compatibility), needed by
  `countDocuments`; index creation calls; `getParameter`-style probes — stub honestly,
  never lie about features).
- [ ] **M6.3** Reactivity check: two browsers on the app; a todo added in one appears in the
  other **without refresh**, and chimera logs show an active tailable cursor on
  `local.oplog.rs` (i.e., oplog driver, not poll-and-diff fallback).
- [ ] **M6.4** The README's headline party trick: `INSERT` a todo via the **`mariadb`
  client** — it appears live in both browsers (trigger → oplog → DDP).
- [ ] **M6.5** Repeat M6.3/M6.4 on the 10.11 build.

**Exit criteria:**
- [ ] Reactive todos on 10.11 **and** 11.8, including the SQL-insert-appears-live demo.

---

## Milestone 7 — Cross-language ergonomics

**Goal:** the two "wrong-direction" query paths promised in the design.

- [ ] **M7.1** SQL from mongo clients: admin command `{chimeraSql: "SELECT …"}` (and a
  `$sql` aggregation stage alias) returning rows as BSON documents. Read-only by default;
  a system variable gates write statements.
- [ ] **M7.2** Mongo syntax from SQL clients: `mongo_find('<db>.<coll>', '<filter-json>')`
  UDF (loadable function linking the translator) returning a JSON array; stored-procedure
  wrappers for insert/update/delete that also write oplog rows.
- [ ] **M7.3** Document both with copy-paste examples in `chimera/README.md`.

**Exit criteria:**
- [ ] Both examples work on both versions; docs updated.

---

## Milestone 8 — Hardening backlog *(explicitly out of scope for v1 — do not start without discussion)*

- [ ] Wire-level transactions (`startTransaction`/`commitTransaction` → InnoDB txns — natural fit)
- [ ] SCRAM-SHA-256 auth on the mongo listener (until then: localhost bind only)
- [ ] Positional `$` update operator; `$elemMatch` completeness
- [ ] Filter compile-to-SQL fast path (replace RMW scans; push predicates to generated-column indexes)
- [ ] `eager` projection mode automation (sampling + auto-ALTER policy)
- [ ] mongodump/mongorestore compatibility pass
- [ ] Strict type-mismatch mode (D8)
- [ ] Performance baseline + regression suite

---

## Testing strategy (summary)

| Layer | Tool | Runs against |
|---|---|---|
| Translator unit tests | ctest (M2) | no server |
| Differential specs | `tests/differential/run.sh` (M4) | chimera 10.11 + 11.8 vs oracle mongod 8.0.12 |
| Oplog demos/units | M5 scripts | both versions |
| End-to-end | Meteor todos (M6) | both versions |
| Hygiene | `check-hygiene.sh` (M2.6) | repo |

`chimera/scripts/test.sh --server <v>` runs the whole pyramid; CI runs it twice (matrix).

## Risks

| Risk | Mitigation |
|---|---|
| extJSON ergonomics for DBAs (dates look like `{"$date":…}`) | Helper SQL functions (`chimera_date(doc,path)` etc.) in `chimera/sql/`; document path syntax in README |
| MariaDB JSON-path vs Mongo array semantics divergence | Differential suite is the referee; unsupported constructs error loudly (M2.4) |
| Plugin API drift between 10.11 and 11.8 | Version guards kept minimal + commented; CI matrix catches breakage on every change |
| SQL injection via generated WHERE clauses | Bind parameters only (M2.4); code review gate |
| Meteor driver expectations beyond the plan | M6 gap-list process; stub honestly, never advertise unimplemented features in `hello` |
| Oplog table growth | M5.4 pruning knobs; monitor row count in tests |
