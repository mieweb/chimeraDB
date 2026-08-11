# chimeraDB Implementation Plan

**Date:** 2026-08-10
**Workspace:** `/Volumes/Case/prj/chimeraSQL`
**Goal:** A MongoDB-compatible document store implemented *inside* MariaDB — single storage
engine (InnoDB), single transaction domain — good enough to run a Meteor.js app
(oplog tailing included), with documents auto-projected into relational columns.
**Server targets:** MariaDB **10.11.18 (LTS)** and **11.8.8 (LTS)** — both already built and
verified locally (see [build-plan.md](build-plan.md)); every milestone's exit criteria must
pass on **both**.

> **Doc map (DRY):** [README.md](README.md) owns the *what & why* — product pitch,
> architecture diagram, compatibility surface, licensing/trademark statement.
> [build-plan.md](build-plan.md) owns the base-binary build recipes.
> [release-plan.md](release-plan.md) owns M9 — packaging, distribution, CI — and the tickets
> for the M8 items that became projects of their own. This file owns the
> *how & when* — engineering decisions, milestones, exit criteria — and does not restate
> the others. The `mongodb/` tree (r8.0.12) is used **only** as a reference and for
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
| Cross-language gateways — `mongo('db.users.findOne(…)')` verbatim gateway, `chimeraSql`/`$sql` | plugin + translator | M7 |
| Meteor acceptance harness | `chimera/tests/meteor/` | M6 |

### Locked decisions

| # | Decision | Rationale |
|---|---|---|
| D1 | One engine (InnoDB), two protocol heads | Only shape where doc+projection writes are atomic; mongod exposes no XA so dual-engine can never be transaction-safe |
| D2 | Compatibility bar = **Meteor.js** | Oplog tailing is the linchpin (hello presents a single-node replica set). The **authoritative** supported / not-supported command surface is the single table in [README § Compatibility](README.md#compatibility) — milestones implement exactly that list, nothing more |
| D3 | Document is **source of truth**; **forward** projections are `GENERATED ALWAYS` columns | Engine-enforced: forward columns cannot drift. `ALTER … ADD COLUMN … AS (JSON_VALUE(doc,'$.path')) PERSISTENT` backfills natively (the ALTER rebuild *is* the scan) |
| D4 | Projection mode per collection: `manual` (default) \| `eager` \| `lazy` | Manual = DBA issues ALTERs. Eager = auto-ALTER on new paths (later). Lazy = no physical columns, JSON_VALUE on demand |
| D5 | Storage encoding = **MongoDB Extended JSON (canonical)** in a `JSON` column | MariaDB JSON is text; extJSON preserves BSON types (ObjectId, Date, Timestamp, Decimal128). libbson converts both directions for free |
| D6 | Update execution = **read-modify-write** (doc locked `FOR UPDATE`, ops applied via libbson in memory, full doc written back) | KISS: correct for *all* update operators under InnoDB row locking. Compile-to-SQL is a later optimization, not a requirement |
| D7 | Oplog = InnoDB table written **in the same transaction** as the mutation; triggers on collection tables catch raw-SQL writes | Transactional oplog (never shows rolled-back writes) — stronger than real MongoDB. In-process commit notification wakes tailing cursors |
| D8 | Type-mismatch policy: permissive (`JSON_VALUE` → NULL + warning) by default; strict mode later | Matches MariaDB semantics; documented knob |
| D9 | All chimera code is GPLv2, out-of-tree, in `chimera/` | Zero patches to either upstream. Outbound licensing & trademark statement: [README § License & trademarks](README.md#license--trademarks) |
| D10 | Projection **direction** per column: `forward` (default) \| `bidirectional` (real column + generated `BEFORE` write-through trigger, so plain `UPDATE t SET col=…` becomes `JSON_SET` on the doc) | Makes `UPDATE users SET name=…` legal SQL, opt-in. Doc wins if one statement changes both doc and column; SQL `INSERT` still supplies `doc`. Naked mongosh at the SQL prompt stays impossible without a parser fork (rule 2) — the `mongo('…')` verbatim gateway (M7.2) is the supported form. Promise documented in [README § One prompt, both languages](README.md#one-prompt-both-languages) |
| D11 | The plugin is **only ever built by the server's own CMake** (`link-plugin.sh` + `MYSQL_ADD_PLUGIN`); packages configure a MariaDB source tree and build the `chimera_mongo` target alone | Measured, not assumed ([M9.1](release-plan.md#m91--the-structural-decision-building-a-plugin-with-no-server-source-tree)): a standalone build against `libmariadbd-dev` is impossible, because `mongogateway_udf.cc` needs `sql_class.h` for the caller's current database and no plugin service exposes it. It is also unnecessary — the plugin target's dependency subgraph is mysys/strings/GenError, so a *fresh* build directory reaches `chimera_mongo.so` in 222 targets and 19s, not a server build. One build path means the packaged artifact and the developer's are the same object by construction, and no CI referee is needed to keep them that way |

### Non-negotiable ground rules

1. **SSPL hygiene:** never copy, port, or paraphrase code from `mongodb/` into `chimera/`.
   The mongodb tree is a *black-box* reference (the "test oracle", in differential-testing
   terms) and client binary only. Implement the wire
   protocol from public documentation and observed behavior (FerretDB proved cleanroom
   viability). A CI check greps `chimera/` for `mongodb/src` includes — it must stay empty.
2. **Zero upstream patches:** nothing inside the `mariadb-server/` (11.8) or `mariadb-10.11/`
   trees may be edited except a regenerable symlink into `plugin/`. If a server patch ever
   seems necessary, **stop and escalate** — that changes the maintainability story.
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
├── mariadb-10.11/              # 10.11.18 tree + build/ (gitignored)
├── mongodb/                    # r8.0.12 — REFERENCE + mongo shell ONLY (gitignored)
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
| reference `mongod` | 27117 |

---

## Milestone 0 — Dual-version scaffolding

**Goal:** both MariaDB versions build and run via scripts; `chimera/` skeleton exists.

> **As-built (2026-08-10):** both server binaries already exist and are verified — see
> [build-plan.md](build-plan.md) Track A (11.8) and Track A2 (10.11). M0.3/M0.4 are done;
> what remains for M0 is the installed layout, the run scripts, and the `chimera/` skeleton.
>
> | Server | Tree | Binary | Version |
> |---|---|---|---|
> | 11.8 LTS | `mariadb-server/` | `mariadb-server/build/sql/mariadbd` | `11.8.8-MariaDB` (client `15.2`) |
> | 10.11 LTS | `mariadb-10.11/` | `mariadb-10.11/build/sql/mariadbd` | `10.11.18-MariaDB` (client `15.1`) |
> | reference | `mongodb/` | `mongodb/build/install/bin/mongod` + `bin/mongo` | `8.0.12` |

- [x] **M0.1** Create the `chimera/` directory skeleton above with a `README.md` explaining
  the folder's organizing principle (one paragraph per subdirectory).
- [x] **M0.2** ✅ `/mariadb-10.11/` added to [.gitignore](.gitignore) alongside the other clones.
- [x] **M0.3** ✅ 10.11 lives in its own **shallow clone** `mariadb-10.11/` at tag
  `mariadb-10.11.18` (a `git worktree` off `mariadb-server/` isn't possible — that clone is
  `--depth 1` and has no 10.11 history). Submodules initialized `--depth 1`.
  Configured and built with the identical Track A recipe (built clean on the first try,
  1947/1947 targets, no source patches):
  ```sh
  export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
  export SDKROOT=$(xcrun --sdk macosx --show-sdk-path)
  export PATH="$(brew --prefix bison)/bin:$PATH"
  cd mariadb-10.11 && cmake -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DPLUGIN_COLUMNSTORE=NO -DPLUGIN_ROCKSDB=NO \
    -DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3) \
    -DWITH_SSL=$(brew --prefix openssl@3) \
    -DCMAKE_OSX_SYSROOT="$SDKROOT" \
    -DLIBXML2_INCLUDE_DIR="$SDKROOT/usr/include/libxml2" \
    -DZLIB_INCLUDE_DIR="$SDKROOT/usr/include"
  cmake --build build -j12
  ```
  The two explicit `*_INCLUDE_DIR` overrides matter: without them CMake injects a stale
  Command Line Tools SDK include path that shadows libc++ headers (the failure documented
  in [build-plan.md](build-plan.md) Track A).
- [x] **M0.4** ✅ Both trees report the expected versions:
  ```sh
  mariadb-server/build/sql/mariadbd --version   # 11.8.8-MariaDB for osx10.21 on arm64
  mariadb-10.11/build/sql/mariadbd --version    # 10.11.18-MariaDB for osx10.21 on arm64
  ```
- [x] **M0.5** Create an installed layout for each tree (needed for `mariadb-install-db`,
  plugin dirs, and mtr later — this is the gap that blocked the earlier smoke test):
  ```sh
  cmake --install mariadb-server/build --prefix "$PWD/mariadb-server/dist"
  cmake --install mariadb-10.11/build --prefix "$PWD/mariadb-10.11/dist"
  ```
- [x] **M0.6** Write `chimera/scripts/run-server.sh --server 10.11|11.8 [--fresh]` and
  `stop-server.sh`: init datadir with `mariadb-install-db` on first run, start `mariadbd`
  with the port conventions above, pidfile under `chimera/.run/`. Never hardcode paths —
  derive from `--server`.
- [x] **M0.7** Install the Apache-2.0 BSON/Mongo C libraries used by the translator:
  ```sh
  brew install mongo-c-driver
  pkg-config --list-all | grep -iE 'bson|mongoc'   # note the pkg names (1.x: libbson-1.0; 2.x: bson2)
  ```
- [x] **M0.8** JSON feature probe on both servers (via `mariadb` client against each):
  `SELECT JSON_VALUE('{"a":{"b":2}}','$.a.b');` returns `2`;
  `CREATE TEMPORARY TABLE t (d JSON, v INT AS (JSON_VALUE(d,'$.x')) VIRTUAL);` succeeds.
- [x] **M0.9** ✅ Plugin precedents confirmed present in **both** trees:
  `plugin/daemon_example/`, `plugin/handler_socket/`, `plugin/test_sql_service/`.

**Exit criteria:**
- [x] `run-server.sh --server 11.8` and `--server 10.11` start clean servers; probe SQL passes on both; skeleton committed.

**Status:** complete. Both servers build, install, start and stop from
[chimera/scripts/](chimera/scripts/), the JSON probe passes on each, and the `chimera/`
skeleton is committed.

---

## Milestone 0.5 — CONNECT↔mongod sandbox *(optional, timeboxed)*

**Goal:** zero-code preview of "SQL over live Mongo documents" to inform projection
ergonomics, and a future data-migration bridge. Skippable without affecting later milestones.

> Caveat: the CONNECT engine's MONGO table type requires **libmongoc-1.0** (driver 1.x)
> at configure time ([storage/connect/CMakeLists.txt#L341](mariadb-server/storage/connect/CMakeLists.txt#L341)).
> If brew installed driver 2.x in M0.7, either install a 1.x driver alongside or skip this milestone.

- [ ] **M0.5.1** Reconfigure the 11.8 tree so CMake reports `CONNECT_MONGODB: ON`; rebuild the CONNECT plugin only.
- [ ] **M0.5.2** Start the reference mongod (port 27117) and seed a few documents via the built `mongo` shell.
- [ ] **M0.5.3** In the 11.8 server: `INSTALL SONAME 'ha_connect';` then
  `CREATE TABLE users ENGINE=CONNECT TABLE_TYPE=MONGO TABNAME='test.users' CONNECTION='mongodb://127.0.0.1:27117';`
  (no column list → exercises **discovery**: CONNECT samples documents and infers columns).
- [ ] **M0.5.4** Record findings in `chimera/README.md` (§ prior art): how nested paths flatten,
  how types map, what discovery gets wrong. These observations feed D4/D5 defaults.

**Exit criteria:**
- [x] Findings paragraph committed (or milestone explicitly marked skipped here).

> **Correction (2026-02-14, M0.5 — skipped, taking the escape hatch this milestone offers):**
> M0.7 installed mongo-c-driver 2.x, and CONNECT's MONGO table type needs libmongoc **1.x**
> at configure time. Installing a second driver alongside it to preview an ergonomics
> question — on an engine ChimeraDB does not use — buys less than it costs, and the milestone
> was written to be skippable. The projection defaults it would have informed (D4/D5) were
> settled empirically instead, by M1's demo and M5's `chimera_add_projection`.

---

## Milestone 1 — Doc-store core, pure SQL (no C++ yet)

**Goal:** prove the storage model end-to-end with plain SQL on both versions.

- [x] **M1.1** Write `chimera/sql/catalog.sql`:
  - `CREATE DATABASE IF NOT EXISTS chimera_meta;`
  - `chimera_meta.collections(db_name, coll_name, projection_mode ENUM('manual','eager','lazy') DEFAULT 'manual', created_at, PRIMARY KEY(db_name, coll_name))`
- [x] **M1.2** Document the collection table convention in `chimera/README.md`:
  - Mongo database ⇒ MariaDB database; collection ⇒ table.
  - `CREATE TABLE <db>.<coll> (_id VARBINARY(255) NOT NULL PRIMARY KEY, doc JSON NOT NULL) ENGINE=InnoDB;`
    (MariaDB's `JSON` alias adds the `JSON_VALID` check automatically.)
  - `_id` holds the canonical byte form produced by the translator (M2); for now use plain strings.
- [x] **M1.3** Write `chimera/scripts/demo-m1.sh --server <v>` executing this walkthrough
  and asserting each result:
  - [x] create a `test.users` collection table + catalog row
  - [x] insert two extJSON documents (one with `{"$date": ...}` field)
  - [x] update one via `UPDATE … SET doc = JSON_SET(doc, '$.age', 31)`
  - [x] `ALTER TABLE test.users ADD COLUMN email VARCHAR(190) AS (JSON_VALUE(doc,'$.email')) PERSISTENT, ADD INDEX(email);`
        — then `SELECT email FROM test.users` proves the **backfill happened during the ALTER** (D3)
  - [x] add a `VIRTUAL` + indexed column too; note no rebuild occurred
  - [x] prove drift is impossible: `UPDATE test.users SET email='x'` → error (generated column)
  - [x] type-mismatch probe (D8): declare an `INT` projection over a string path → NULL + warning captured
  - [x] nested/typed path example: `JSON_VALUE(doc, '$.createdAt."$date"')` extracts the extJSON date
- [x] **M1.4** Bidirectional write-through prototype (D10), hand-rolled: make `name` a
  **real** `VARCHAR(190)` column plus a `BEFORE UPDATE` trigger — if `doc` changed,
  recompute `name` from it; otherwise if `name` changed, `JSON_SET` it into `doc`
  (doc wins when both change in one statement). Acceptance is literally the README
  example: `UPDATE test.users SET name = 'Douglas Horner' WHERE email = 'doug@example.com';`
  then assert `JSON_VALUE(doc, '$.name')` changed with it. Add to `demo-m1.sh`.
- [x] **M1.5** Run the demo against **both** servers and fix any 10.11/11.8 divergence found
  (record divergences in `chimera/README.md`).

**Exit criteria:**
- [x] `demo-m1.sh` green on 10.11 and 11.8.

---

## Milestone 2 — Translator library (standalone C++, no server)

**Goal:** the shared brain used later by both the wire plugin and the UDF gateway.
Lives in `chimera/translator/`, builds with its own CMake, tests with ctest.

- [x] **M2.1** Scaffold `chimera/translator/` (CMake, pkg-config for libbson — support both
  1.x and 2.x pkg names) + a unit-test target (single-header framework such as doctest).
- [x] **M2.2** **Codec module:** BSON ⇄ canonical Extended JSON using libbson's built-ins
  (`bson_as_canonical_extended_json` / `bson_new_from_json`). Round-trip tests covering
  ObjectId, Date, Timestamp, Decimal128, Binary, nested arrays.
- [x] **M2.3** **`_id` canonicalization:** ObjectId | string | int → deterministic bytes for
  the `VARBINARY(255)` PK, and back. Covers Meteor random-string ids *and*
  `idGeneration:'MONGO'` ObjectIds. Property test: encode→decode is identity; ordering is stable.
- [x] **M2.4** **Filter compiler** (Meteor/minimongo subset): implicit `$eq`, `$gt/$gte/$lt/$lte/$ne`,
  `$in/$nin`, `$and/$or/$not`, `$exists`, `$regex`, basic `$elemMatch` → parameterized SQL
  `WHERE` over `JSON_VALUE`/`JSON_EXTRACT`/`JSON_CONTAINS` on `doc`. Unsupported operator ⇒
  clean, specific error (fail fast; no silent wrong answers). Every generated fragment uses
  bind parameters — **no string interpolation of user values** (injection surface).
- [x] **M2.5** **Update engine** (per D6): apply `$set/$unset/$inc/$push/$pull/$addToSet/$pop`
  (positional `$` deferred to backlog) to a BSON doc **in memory**; returns new doc + a
  changed-fields summary (used by the oplog writer in M5). Unit tests mirror MongoDB's
  documented semantics for each operator, including edge cases (missing paths, arrays).
- [x] **M2.6** Hygiene gate: `chimera/scripts/check-hygiene.sh` — fails if anything under
  `chimera/` includes or references `mongodb/src` (rule 1). Wire into `test.sh`.

**Exit criteria:**
- [x] `ctest` green; hygiene gate green. (Server-independent — no dual-version matrix here.)

---

## Milestone 3 — Daemon plugin skeleton (wire listener, handshake)

**Goal:** `mongo` shell connects to mariadbd and can `ping` — on both server versions.

- [x] **M3.1** Read the two in-tree precedents before writing code:
  [plugin/daemon_example](mariadb-server/plugin/daemon_example) (minimal daemon plugin
  lifecycle) and [plugin/handler_socket](mariadb-server/plugin/handler_socket) (a plugin
  running its own network listeners). Note how `st_maria_plugin` is declared, and how
  init/deinit manage threads.
- [x] **M3.2** Create `chimera/plugin/chimera_mongo/` with `CMakeLists.txt` using
  `MYSQL_ADD_PLUGIN(chimera_mongo … MODULE_ONLY)`; declare `PLUGIN_LICENSE_GPL` and
  `MariaDB_PLUGIN_MATURITY_EXPERIMENTAL`. Link the translator static lib + libbson.
- [x] **M3.3** Write `chimera/scripts/link-plugin.sh` — symlinks the plugin dir into each
  server tree's `plugin/` (server CMake auto-discovers subdirectories) and re-runs cmake:
  ```sh
  ln -sfn "$PWD/chimera/plugin/chimera_mongo" mariadb-server/plugin/chimera_mongo
  ln -sfn "$PWD/chimera/plugin/chimera_mongo" mariadb-10.11/plugin/chimera_mongo
  ```
  This symlink is the **only** thing that ever touches the server trees (rule 2).
  Expect the plugin to need `#if MYSQL_VERSION_ID` guards for 10.11 vs 11.8 API drift —
  keep them few and commented.
- [x] **M3.4** Plugin skeleton: system variables `chimera_mongo_port` (defaults per port
  table), `chimera_mongo_bind` (**default `127.0.0.1`** — no auth exists yet, never bind
  wide by default); listener thread started in plugin init, joined in deinit (server must
  shut down clean, no leaked threads).
- [x] **M3.5** Wire framing: implement **OP_MSG**, *plus* legacy **OP_QUERY only for the
  initial `isMaster`/`hello` handshake* — drivers and the legacy shell send their first
  handshake as OP_QUERY before switching to OP_MSG. Reply with `OP_REPLY` for that one
  path. Everything else is OP_MSG-only.
- [x] **M3.6** Commands: `hello`/`isMaster` (present as single-node replica set:
  `isWritablePrimary:true`, `setName:"chimera"`, `me`/`hosts`, `logicalSessionTimeoutMinutes:30`,
  `maxWireVersion:17`, `minWireVersion:0` — document why 17), `ping`, `buildInfo`,
  `endSessions` (accept + no-op), and a proper error envelope (`ok:0, code, codeName, errmsg`).
- [x] **M3.7** Manual verification with the built reference shell against **both** servers:
  ```sh
  mongodb/build/install/bin/mongo --port 27018 --quiet --eval 'db.runCommand({ping:1})'
  mongodb/build/install/bin/mongo --port 27019 --quiet --eval 'db.runCommand({ping:1})'
  ```
- [x] **M3.8** `chimera/scripts/build-plugin.sh --server <v>` + extend `run-server.sh` to
  `INSTALL SONAME 'chimera_mongo'` (or `--plugin-load-add`) automatically.

**Exit criteria:**
- [x] Shell connects, `ping` and `hello` return well-formed replies on 10.11 **and** 11.8; clean server shutdown.

---

## Milestone 4 — CRUD over the wire

**Goal:** the Meteor CRUD surface works, verified differentially against real mongod.

- [x] **M4.1** Execute SQL from inside the plugin via the server's **SQL service** —
  study [plugin/test_sql_service](mariadb-server/plugin/test_sql_service) first; all
  chimera SQL runs through one internal helper (single choke point for txn control + binds).

  > **Correction (2026-08-10):** the SQL service has **no prepared statements** — it offers
  > `mysql_real_query` and `mysql_real_escape_string` and nothing else. "Binds" therefore
  > became `SqlSession::render()`, a single audited substitution point: every value reaches
  > SQL through `quote()`/`quote_binary()`, so there is still exactly one thing to review.
  >
  > Three further facts the header does not advertise. A plugin thread must call
  > `my_thread_init()`/`my_thread_end()` before it owns a session (`SqlThreadScope`) —
  > `THD::store_globals()` dereferences `my_thread_var` immediately. The local connection
  > builds its own `THD` with **no current database**, so every statement is fully qualified.
  > And on macOS the plugin needs `-Wl,-undefined,dynamic_lookup`, because those mysys
  > symbols live in the `mariadbd` executable rather than in any linkable library and
  > Apple's linker no longer implies dynamic lookup for `-bundle`.

- [x] **M4.2** Commands, each with its own checkbox and differential spec file:
  - [x] `create` (collection) → table DDL + catalog row (+ triggers placeholder for M5)
  - [x] `insert` (ordered batches, duplicate-`_id` → code 11000 `DuplicateKey`)
  - [x] `find` with filter/projection/sort/limit/skip/batchSize → cursor machinery
  - [x] `getMore` / `killCursors` (cursor registry with timeouts)
  - [x] `update` (multi, upsert; RMW per D6 inside one InnoDB txn per doc batch)
  - [x] `delete` (single + multi)
  - [x] `findAndModify`
  - [x] `count`, `distinct`
  - [x] `listDatabases`, `listCollections`, `listIndexes`
  - [x] `createIndexes` → `ALTER TABLE … ADD COLUMN … AS (JSON_VALUE(doc,…)) VIRTUAL, ADD INDEX`
        honoring the collection's projection mode; `dropIndexes`
  - [x] `drop`, `dropDatabase`
  - [x] implicit sessions: accept & ignore `lsid`/`txnNumber` on all of the above

  > **Correction (2026-08-10):** three behaviours the plan assumed differ from the reference.
  > A write batch is one transaction **per element**, not per batch — MongoDB batches are
  > not atomic, and rolling back the batch would discard writes the client was already
  > told succeeded. Sort, skip, limit and projection are applied **in the plugin**, not in
  > SQL: `JSON_VALUE` cannot order through canonical extJSON wrappers, and Mongo's
  > cross-type ordering is BSON-specific. (M8's "filter compile-to-SQL fast path" is where
  > that changes.) And `create`/`drop` are idempotent on MongoDB 8.0 while `createIndexes`
  > now *requires* an explicit `name` — the server no longer derives `sku_1_bin_1`.

- [x] **M4.3** Differential harness `chimera/tests/differential/run.sh --server <v>`:
  starts reference mongod (27117) + chimera; runs each `.js` spec through the reference `mongo`
  shell against **both** endpoints; normalizes (`$clusterTime`, `operationTime`, cursor ids,
  key order) and diffs. A spec passes only if outputs match.
- [x] **M4.4** Error-parity specs: unknown collection, bad filter operator, duplicate key —
  same `code`/`codeName` as the reference where Meteor depends on them.

  > **Correction (2026-08-10):** parity is asserted on `code`/`codeName`, never on message
  > prose, which is not contractual. Matching the reference forced three code changes: an
  > operator we simply do not recognize is `BadValue` (2) rather than `NotImplemented`,
  > which stays for the known-but-deferred cases; an unknown update modifier is
  > `FailedToParse` (9); and a missing required command field is `IDLFailedToParse`
  > (40414), which is what MongoDB's generated parsers emit.

**Exit criteria:**
- [x] Differential suite green on 10.11 **and** 11.8.

> **Correction (2026-08-10):** two bugs that only the differential suite could have found,
> both invisible to the unit tests because those assert on generated SQL *strings*.
>
> **M2's range comparisons never worked.** `JSON_VALUE` returns NULL on a wrapped scalar
> like `{"$numberInt":"10"}`, so `$gt`/`$lt` matched nothing while every string-shape test
> stayed green. Fixed by `scalar_expr()`, a `COALESCE` over the canonical extJSON wrappers,
> now shared with the generated index columns so the index computes exactly what the WHERE
> clause computes.
>
> **Every filter, sort and projection was silently empty.** `bson_init_static` points a
> `bson_t` at its *own* storage, and `bson_iter_value` hands back a value owned by a
> temporary — so a view taken from either and returned by value dangles. All queries
> matched all documents. Views now come from `view_field()`, which borrows directly from
> the request body and never copies.
>
> `_id` also had to learn about doubles: a JavaScript client sends every unadorned number
> as one, so *every* `{_id: 1}` insert was being rejected. Integral doubles fold onto the
> integer tag, so `1`, `1.0` and `NumberLong(1)` are a single key.

---

## Milestone 5 — Oplog + tailable cursors (the Meteor enabler)

**Goal:** `local.oplog.rs` emulation with transactional guarantees (D7).

- [x] **M5.1** Schema in `chimera/sql/oplog.sql`: `chimera_meta.oplog(seq BIGINT AUTO_INCREMENT PK, ts_t INT UNSIGNED, ts_i INT UNSIGNED, op ENUM('i','u','d'), ns VARCHAR(512), o JSON, o2 JSON NULL)`;
  Timestamp rule: `ts_t` = unix seconds, `ts_i` = per-second counter derived under the same lock as `seq`.
- [x] **M5.2** Translator/plugin write path: every insert/update/delete appends its oplog row
  **in the same transaction** ('u' entries use full-document replacement style in `o`,
  `{_id}` in `o2` — simplest form Meteor accepts).
- [x] **M5.3** SQL-side capture: `chimera/sql/triggers.tpl.sql` — AFTER INSERT/UPDATE/DELETE
  triggers per collection table appending equivalent oplog rows; installed by `create`
  (M4.2) and by a `chimera_adopt_table` procedure for pre-existing tables. Guard against
  double-write when the mutation came through the plugin (session variable flag).
- [x] **M5.4** Capped-collection emulation: background pruning of `chimera_meta.oplog`
  by age/row-count (plugin timer thread; both knobs are system variables).
- [x] **M5.5** Tailable + `awaitData` cursors on `local.oplog.rs`: map the namespace to the
  oplog table; support Meteor's exact query shapes (`ts: {$gt: <Timestamp>}`, ns filtering,
  initial "latest entry" fetch); `getMore` parks on an in-process condition variable
  signaled at commit (no polling), honoring `maxTimeMS`.
- [x] **M5.6** Demo script `chimera/scripts/demo-oplog.sh --server <v>`: shell A tails the
  oplog; shell B inserts via the wire; a third write goes through the **`mariadb` SQL
  client** — all three appear on the tail, in commit order.
- [x] **M5.7** Unit-test Meteor's oplog query shapes and Timestamp round-tripping.
- [x] **M5.8** Productize bidirectional projections (D10): `chimera_add_projection('<db>.<coll>',
  '<json-path>', '<column>', '<type>', 'forward'|'bidirectional')` procedure — forward
  emits the `GENERATED ALWAYS` ALTER; bidirectional emits a real column + backfill
  `UPDATE` + regenerated `BEFORE` write-through trigger, ordered so the AFTER oplog
  trigger (M5.3) captures the post-write-through doc.

> **Correction (2026-02-14, M5):** the triggers of M5.3 turned out to be the *only*
> oplog writer. A plugin-side append (M5.2) would have to be suppressed whenever a
> trigger also fired, and the two paths would drift; making the trigger authoritative
> deletes both the second code path and the double-write guard variable. M5.2 is
> therefore satisfied *by* M5.3 rather than alongside it — the plugin's writes are
> ordinary SQL, so their oplog rows land in the same transaction for free.

> **Correction (2026-02-14, M5.1):** oplog entry documents are assembled with `CONCAT`,
> not `JSON_OBJECT`. MariaDB's JSON type is LONGTEXT, so `JSON_OBJECT('o', o)` embeds
> the stored document as a *string*. Total ordering of `(ts_t, ts_i)` comes from an X
> lock on a single `chimera_meta.oplog_clock` row, which serializes writers — the price
> of a monotonic timestamp without a replica set's oplog allocator.

> **Correction (2026-02-14, M5.3):** all of this runs as a dedicated locked account,
> `'chimera'@'localhost'`. The plugin's local connection is the server's anonymous
> internal user, so anything it creates gets definer `''@''` — and a trigger whose
> definer does not exist *refuses to fire* (`ERROR 1449`), which silently breaks the
> raw-SQL write path that is the whole point of trigger-based capture. The account is
> created with `ACCOUNT LOCK` and granted only `SELECT, TRIGGER` globally (a trigger
> body reading `NEW.doc` is privilege-checked against its definer, and collection
> tables appear in any database on demand) plus DML on `chimera_meta`.

> **Correction (2026-02-14, M5.5):** `awaitData` parks on the condition variable as
> planned, but caps each wait at ~50 ms and re-queries. A write made by an ordinary
> SQL client happens outside the plugin's process and cannot signal its condvar, so a
> pure condition-variable wait would miss exactly the writes M5.6 exists to prove. The
> polling interval is the latency floor for SQL-side writes only; wire writes are still
> signaled immediately at commit. A trigger-invoked notify UDF would remove the poll.

> **Correction (2026-02-14, M5.8):** `chimera_add_projection` accepts only a single
> top-level field, not a JSON path — a nested path needs a merge patch the procedure
> does not build, and projecting the wrong subtree silently is worse than refusing. Two
> MariaDB constraints shaped the implementation: stored functions are forbidden inside
> generated-column expressions, so the extJSON type-wrapper `COALESCE` is spelled out
> at each use site rather than factored out; and `JSON_MERGE_PATCH` descends into
> objects, so the field's old type wrapper must be `JSON_REMOVE`d before the new one is
> merged, or the document ends up carrying both. Bidirectional mode also issues a
> column-scoped `GRANT UPDATE (doc, <column>)` on that one table, because a BEFORE
> trigger assigning `NEW.doc` is checked against its definer.

**Exit criteria:**
- [x] `demo-oplog.sh` shows wire-writes *and* raw-SQL writes streaming to a tailing cursor, on both versions.
- [x] A bidirectional-column `UPDATE` (M5.8) produces exactly one oplog `'u'` entry containing the merged document.

> **Sequel (2026-08-10):** Meteor 3.5 flipped its default reactivity driver from oplog
> tailing to MongoDB **change streams** (`changeStreams → oplog → polling`), and chimera's
> `hello` advertises exactly the signals (`setName`, wire version 17) that make a 3.5
> driver select change streams — then fail at `watch()`. Serving `$changeStream` from
> this same oplog, plus the stopgap for stock Meteor 3.5 until it lands, is specced with
> checkboxes in **[changestream-plan.md](changestream-plan.md)**.

---

## Milestone 6 — Meteor end-to-end (acceptance test)

**Goal:** a stock Meteor todos app runs reactively against chimera. This is the bar.

- [x] **M6.1** `chimera/tests/meteor/`: install Meteor, scaffold the standard todos example,
  script `run-meteor.sh --server <v>` exporting:
  ```sh
  export MONGO_URL="mongodb://127.0.0.1:27018/meteor"
  export MONGO_OPLOG_URL="mongodb://127.0.0.1:27018/local"
  ```
- [x] **M6.2** Fix the gap list until startup is clean (expected suspects: the aggregation
  subset promised in [README § Compatibility](README.md#compatibility), needed by
  `countDocuments`; index creation calls; `getParameter`-style probes — stub honestly,
  never lie about features).
- [x] **M6.3** Reactivity check: two browsers on the app; a todo added in one appears in the
  other **without refresh**, and chimera logs show an active tailable cursor on
  `local.oplog.rs` (i.e., oplog driver, not poll-and-diff fallback).
- [x] **M6.4** The README's headline party trick: `INSERT` a todo via the **`mariadb`
  client** — it appears live in both browsers (trigger → oplog → DDP).
- [x] **M6.5** Repeat M6.3/M6.4 on the 10.11 build.

> **Correction (2026-02-14, M6.2 — the bug a real driver found and the harness did not):**
> chimera answered every write with `writeErrors: []`. mongod *omits* the field when a
> batch succeeded, and the Node driver treats its presence as proof there is a first error:
> it runs `new MongoServerError(res.writeErrors[0])` and dies on `undefined.message` before
> the insert can return. Every Meteor boot crashed on its seed data. The array is now built
> to one side and attached only when non-empty. Eight differential specs against a live
> mongod never caught this — the transcripts compare *documents returned*, not reply shape,
> so a field that is always present and always empty diffs clean. Acceptance tests earn
> their keep on exactly this class of bug.

> **Correction (2026-02-14, M6.2 — the aggregation split):** a pipeline is cut at its first
> reshaping stage. Leading `$match`es merge into one `{$and: […]}` and compile to SQL; every
> later stage runs in this process over documents already loaded. A `$match` *after* a
> reshaping stage raises `not_implemented` rather than being pushed down (it would filter on
> fields that no longer exist) — an unsupported stage is always an error, never a silent
> no-op, because a dropped stage returns the wrong answer. The implemented set is exactly
> what [README § Compatibility](README.md#compatibility) advertises and no more.

> **Correction (2026-02-14, M6.2 — handshake stubs):** `replSetGetStatus` reports a
> permanent one-member set that is always `PRIMARY`; without an answer Meteor never looks
> for an oplog and the whole of M5 goes unused. `getParameter` answers only
> `featureCompatibilityVersion` and simply omits anything else, rather than inventing values
> for knobs chimera does not have.

> **Correction (2026-02-14, M6.1 — the scaffold is not turnkey):** `meteor create --full`
> writes `package.json` but does not install it, so the app dies on a missing
> `@babel/runtime`; and the generated `links.insert` method calls the synchronous
> `Collection.insert` that Meteor 3 removed. Both are scaffold bugs, not chimera bugs, but
> both stop the demo — `run-meteor.sh` therefore runs `meteor npm install` and patches the
> method to `insertAsync`, so the fix lives in a tracked script rather than in an untracked
> `.run/` directory.

> **Correction (2026-02-14, M6.3/M6.4 — what "two browsers" is evidence of):** the check
> that matters is not that a second browser refreshes, it is *what woke it up*. With
> `MONGO_OPLOG_URL` set, a page opened before the write received both a wire insert from
> another browser and a raw `INSERT` from the `mariadb` client, live. Poll-and-diff would
> also show the first; only oplog tailing shows the second, because Meteor never issues a
> query that would notice a write it did not make.

**Exit criteria:**
- [x] Reactive todos on 10.11 **and** 11.8, including the SQL-insert-appears-live demo.

---

## Milestone 7 — Cross-language ergonomics

**Goal:** the cross-language paths promised in [README § One prompt, both languages](README.md#one-prompt-both-languages).

- [x] **M7.1** SQL from mongo clients: admin command `{chimeraSql: "SELECT …"}` (and a
  `$sql` aggregation stage alias) returning rows as BSON documents. Read-only by default;
  a system variable gates write statements.
- [x] **M7.2** Verbatim mongosh from SQL clients: a `mongo('<statement>')` gateway (loadable
  function linking the translator) that accepts a pasted shell statement —
  `db.<coll>.<verb>(<relaxed-JSON args>)` — for `find`/`findOne`/`insertOne`/`insertMany`/
  `updateOne`/`updateMany`/`replaceOne`/`deleteOne`/`deleteMany`/`countDocuments`/`aggregate`.
  `SELECT mongo(…)` returns JSON; write verbs run through the translator so they hit the
  oplog exactly like wire writes. Argument parsing follows mongosh string semantics
  (single- **or** double-quoted strings, unquoted keys). Test under default *and*
  `ANSI_QUOTES` sql_mode — the double-quoted-outer form legitimately breaks under
  `ANSI_QUOTES` (identifier quoting) and must fail with a clear error, per
  [README § One prompt, both languages](README.md#one-prompt-both-languages) quoting rules.
  (Naked, unquoted mongo syntax at the SQL prompt would
  require forking the server parser — ground rule 2 forbids it. The string wrapper is the
  supported form; `chimerash` in M8 is the native-REPL answer.)
- [x] **M7.3** Document both with copy-paste examples in `chimera/README.md`.

> **Correction (2026-02-14, M7.1 — read-only is enforced twice, and neither half is
> redundant):** the keyword whitelist and `START TRANSACTION READ ONLY` catch different
> things. A read-only transaction does not stop DDL, because `CREATE`/`DROP` commit
> implicitly *before* they run and so never become part of the transaction; a keyword check
> cannot know what a view or a trigger does once the statement is underway. `WITH` is
> deliberately absent from the whitelist — MariaDB allows a CTE in front of `UPDATE` and
> `DELETE`, so the first keyword stops being evidence. The check lives in the translator
> (`sqlguard.cpp`) rather than the plugin precisely so a security control can be
> unit-tested without a server.

> **Correction (2026-02-14, M7.1 — `$sql` is a source, not a filter):** `{$sql: …}` may only
> be the first stage, because it produces the documents the rest of the pipeline reshapes,
> and `$match` is not available after it. Filtering belongs in the `WHERE` clause, which is
> the reason to reach for the stage at all. Values come back typed — integers as integers,
> `NULL` as `null` — with `DECIMAL` deliberately arriving as a string, since a double would
> discard the precision it was declared for.

> **Correction (2026-02-14, M7.2 — the gateway is a spelling, not a second engine):** the
> plan said "loadable function linking the translator", which suggested a library of its own.
> It ships inside `chimera_mongo.so` instead (`CREATE FUNCTION mongo RETURNS STRING SONAME
> 'chimera_mongo.so'`), and it does not reimplement any verb: each one becomes the command
> document a driver would have sent and goes through `dispatch_command`. That is what makes
> a `mongo('db.c.updateOne(…)')` produce exactly one oplog entry, identical to the wire's —
> which the demo asserts rather than assumes. Cursors are drained before returning, because
> a SQL caller has no way to ask for the next batch and stopping at 101 documents would be a
> silent truncation.

> **Correction (2026-02-14, M7.2 — finding the caller's database):** `mongo('<statement>')`
> uses the database the session is already `USE`-ing, which needs `current_thd` and
> therefore `#define MYSQL_SERVER` — the header exposure is confined to
> `mongogateway_udf.cc`, which contains no logic and no BSON. The two-argument form
> `mongo('<db>', '<statement>')` exists for callers with no current database.
> `find`/`findOne` take a projection as their second argument; sorting and paging are
> `aggregate`'s job, because a chained `.sort()` is a method call and this is a gateway, not
> a JavaScript engine. `ObjectId('…')` is understood; `ISODate` and friends raise rather
> than guess.

**Exit criteria:**
- [x] Both examples work on both versions; docs updated.

---

## Milestone 8 — Post-v1 roadmap *(ordered 2026-08-10; v1 ships without any of it)*

The backlog was discussed and ordered rather than merely kept. Decisions recorded:

- **v1 = M7 + packaging** ([release-plan.md](release-plan.md) M9). The compatibility bar
  stays Meteor (D2), which never opens a transaction — so v1 ships without them, and
  neither `hello` nor the README pretends otherwise.
- **The v1 performance claim is "does not regress," not "competes with mongod."** That is
  what a baseline + regression suite can prove. A competition claim would need a benchmark
  nobody has designed and a reason to win it nobody has articulated.
- **D6's arc finishes with data.** Read-modify-write was a deliberate KISS bet; the fast
  path gets built only if the baseline shows the bet went bad where it matters.

In order:

1. [ ] **Performance baseline + regression suite** — the next engineering milestone after
   M9 (becomes M10 when specced). Insert / update-by-`_id` / update-by-secondary-key
   throughput, find on projected vs unprojected paths, oplog tail latency under write
   load, and the Meteor todos workload — CI-runnable and diffable, not a one-off
   benchmark. Gates item 3.
2. [ ] **Wire-level transactions — [#6](https://github.com/mieweb/chimeraDB/issues/6)**,
   scheduled *soon after v1*: the first driver-facing feature, for
   `session.withTransaction()` users. The structural half already exists (one `SqlSession`
   per connection, held for its lifetime); the work is the txn-aware write path and,
   above all, the driver retry labels (`TransientTransactionError`,
   `UnknownTransactionCommitResult`) that `withTransaction` loops turn on.
   Connection-pinned first cut, divergence documented.
3. [ ] **Filter compile-to-SQL fast path** (replace RMW scans; push predicates to
   generated-column indexes) — only if item 1's numbers demand it. Starting this without
   the baseline is optimizing a rumor.
4. [ ] **Checkbox-sized, no ordering constraints** — each already fails loudly at a fenced
   `not_implemented` and is differentially testable against the reference:
   - Strict type-mismatch mode (D8) — global sysvar first, per-collection only if asked for
   - Positional `$` update operator ([update.cpp](chimera/translator/src/update.cpp))
   - `$elemMatch` with operators ([filter.cpp](chimera/translator/src/filter.cpp))
   - mongodump/mongorestore compatibility pass

Tracked independently of this order:

- **SCRAM-SHA-256 auth — [#5](https://github.com/mieweb/chimeraDB/issues/5)**:
  release-gating for any non-loopback deployment. Stage 1 (enforce rather than advise)
  is done: a non-loopback bind refuses to start without `chimera_mongo_insecure_bind=ON`,
  the opt-in is logged loudly, and `chimeraSql`/`$sql` are disabled on such a bind.
  Stages 2 (SCRAM itself) and 3 (TLS) remain.
- **Spun out as projects**, each blocked on its own decision rather than on engineering:
  vector search [#2](https://github.com/mieweb/chimeraDB/issues/2), `chimerash`
  [#3](https://github.com/mieweb/chimeraDB/issues/3), `eager` projection automation
  [#4](https://github.com/mieweb/chimeraDB/issues/4). Ticket texts:
  [release-plan.md](release-plan.md#tickets--three-m8-items-that-are-their-own-projects).

---

## Testing strategy (summary)

| Layer | Tool | Runs against |
|---|---|---|
| Translator unit tests | ctest (M2) | no server |
| Differential specs | `tests/differential/run.sh` (M4) | chimera 10.11 + 11.8 vs reference mongod 8.0.12 |
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
| Bidirectional projections are trigger-maintained (no `GENERATED ALWAYS` guarantee) | Opt-in per column (D10); triggers generated from templates, never hand-edited; `chimera_verify_projection()` recomputes and reports drift |
