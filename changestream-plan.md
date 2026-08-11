# Change Streams Plan — `$changeStream` served from the M5 oplog

**Status:** specced 2026-08-10, not started.
**Owner:** unassigned.
**Parent:** [chimeraDB-plan.md § Milestone 5](chimeraDB-plan.md#milestone-5--oplog--tailable-cursors-the-meteor-enabler) — this is M5's sequel, not a rewrite of it.

---

## 1. Why this exists (read this first)

Meteor 3.5 (released 2026-06-30) made **MongoDB change streams the default reactivity
driver**: each reactive query picks the first available driver from
`changeStreams → oplog → polling`
([meteor/meteor PR#13787](https://github.com/meteor/meteor/pull/13787),
[PR#14217](https://github.com/meteor/meteor/pull/14217)). Oplog tailing — the thing all of
M5/M6 was built to feed — is now Meteor's *fallback*, not its first choice.

Worse, Meteor decides change streams are available by looking at the **replica-set signal
(`setName`) in the `hello` reply** plus a MongoDB-6+ server version
([mongo_connection.js](https://github.com/meteor/meteor/blob/devel/packages/mongo/mongo_connection.js):
*"`setName` is the replica-set signal"*). Chimera advertises **both**:

- `setName: "chimera"` in [commands.cc](chimera/plugin/chimera_mongo/commands.cc#L144) (needed for oplog tailing, D2/D7)
- `maxWireVersion: 17` (= MongoDB 6.0) and `buildInfo` version `6.0.0-chimera-…`

So a stock Meteor 3.5 app pointed at chimera will *select* the change-stream driver, call
`collection.watch()`, and hit our `aggregate` with a `$changeStream` stage we do not
implement. Per the M6.2 rule an unknown stage raises `not_implemented` — which on the
Meteor side lands in `SharedChangeStream`'s error handler and schedules **restarts in a
backoff loop**, not a clean fallback to oplog (fallback only happens at driver-selection
time). Our Meteor probe app is pinned to `METEOR@3.3.1`, which is why every existing test
is green today.

The good news: **a change stream is a tailing oplog cursor wearing a different document
shape.** M5 already built the hard parts — the transactional oplog, total ordering,
tailable + `awaitData` cursors, commit wake-ups, pruning. This plan re-skins that
machinery to answer `$changeStream`, plus the small amount of `operationTime` plumbing
Meteor's write-fence logic wants.

---

## 2. What Meteor 3.5 actually sends (verified against meteor/meteor `devel`)

Everything below was read out of
[`shared_change_stream.js`](https://github.com/meteor/meteor/blob/devel/packages/mongo/shared_change_stream.js)
and
[`changestream_observe_driver.js`](https://github.com/meteor/meteor/blob/devel/packages/mongo/changestream_observe_driver.js).
This is the *entire* wire surface we must serve — Meteor opens **one change stream per
collection per connection** and does all selector/projection work client-side.

1. **Availability check** (once per connection): server version ≥ 6 and `hello.setName`
   present. Chimera already passes. Nothing to do.
2. **Pin a start time**: `{ ping: 1 }`, reading `operationTime` from the reply. We
   currently answer `ping` without `operationTime` → Meteor falls back to "now" and loses
   its caught-up floor. We will supply it (Phase 4).
3. **Open the stream**: `collection.watch([], opts)`, which the Node driver sends as:
   ```js
   { aggregate: "<coll>", pipeline: [ { $changeStream: {
       fullDocument: "updateLookup",
       fullDocumentBeforeChange: "whenAvailable",
       // exactly one of, on resume/restart:
       startAfter: <resume token>,          // token from a previous event/PBRT
       startAtOperationTime: <Timestamp>    // from the ping above
   } } ], cursor: {} }
   ```
   The pipeline after `$changeStream` is **always empty** — Meteor deliberately filters
   per-driver in-process. We never need `$match`/`$project` inside the stage.
4. **Drain it**: `getMore` on the returned cursor id (tailable/awaitData semantics; the
   driver passes its await timeout as `maxTimeMS`), `killCursors` on stop.
5. **Consume events**. Fields Meteor reads, and *only* these:
   | field | used for |
   |---|---|
   | `_id` (resume token) | stored; sent back verbatim as `startAfter` after an error/close |
   | `operationType` | must be one of `insert`, `update`, `replace`, `delete`; anything else is ignored |
   | `documentKey._id` | the doc identity |
   | `fullDocument` | the *post-image* — Meteor diffs it against its own cache; it does **not** read `updateDescription` |
   | `fullDocumentBeforeChange` | optional (`whenAvailable` tolerates absence) |
   | `clusterTime` | advanced as `lastProcessedOperationTime`; compared against write fences |
6. **Batch bookkeeping**: the Node driver tracks `cursor.postBatchResumeToken` (PBRT)
   from aggregate/getMore replies so it can resume across quiet periods; replies should
   also carry top-level `operationTime`.
7. **Failure protocol**: error code **286 `ChangeStreamHistoryLost`** (or 280) tells
   Meteor the resume point is gone → it drops the token, reopens from a fresh
   `startAtOperationTime`, and re-reads the collection to reconcile
   (`_resyncAfterHistoryLost`). Any *other* error → restart with the same token
   (exponential backoff). We use 286 when a token predates the pruned window.
8. **Write fences**: after a method's write, Meteor records the write reply's
   `operationTime` on the fence and parks until the stream has processed an event with
   `clusterTime >=` it (`_waitUntilCaughtUp`). If write replies carry no
   `operationTime`, the fence has no annotation and resolves immediately — methods can
   return before the subscription reflects their write. Phase 4 closes this.

## 3. What chimera already has (inventory)

| Piece | Where | Reused as |
|---|---|---|
| Transactional oplog `(seq, ts_t, ts_i, op, ns, o, o2)`, totally ordered | [oplog.cc](chimera/plugin/chimera_mongo/oplog.cc) | the event source; `seq` is the resume token, `(ts_t, ts_i)` is `clusterTime` |
| `'u'` rows carry the **full merged post-image** in `o`, `{_id}` in `o2` | M5.2/M5.8 | `fullDocument` for free — exactly what `updateLookup` means |
| Tail cursors: registry `TailState`, re-read-don't-buffer, `advance_tail` head-skip trick | [cursor.h](chimera/plugin/chimera_mongo/cursor.h), [commands.cc `tail_batch`](chimera/plugin/chimera_mongo/commands.cc#L337) | identical `getMore` mechanics for change-stream cursors |
| Commit wake-up + ~50 ms poll floor for external SQL writes (`kOplogPollMs`) | [commands.cc](chimera/plugin/chimera_mongo/commands.cc#L32) | identical latency story |
| Pruning with knobs (M5.4) | [oplog.cc `prune_oplog`](chimera/plugin/chimera_mongo/oplog.cc) | the thing that makes history-lost (286) *possible*; gets one tweak |
| extJSON-in-SQL entry assembly (`kEntryExpr` CONCAT trick) | [oplog.cc](chimera/plugin/chimera_mongo/oplog.cc) | template for the change-event expression |
| `aggregate` dispatch | [commands.cc `cmd_aggregate`](chimera/plugin/chimera_mongo/commands.cc#L479) | where `$changeStream` is intercepted |

## 4. Design in one paragraph

`aggregate` whose first (and only) stage is `$changeStream` on a real collection
namespace opens a **tail cursor over `chimera_meta.oplog` filtered to `ns = "db.coll"`**,
starting after the seq derived from `startAfter`/`resumeAfter`/`startAtOperationTime`
(default: the current head). Each oplog row is rendered — by a `CONCAT` expression in
SQL, sibling to `kEntryExpr` — directly into a change-stream event document. `getMore`
reuses the existing park-on-commit machinery and additionally reports
`postBatchResumeToken` and `operationTime`. A resume token older than the retained
window raises error 286 so Meteor resyncs itself. Divergences are documented and loud,
never silent.

**Event mapping** (one oplog row → one event; regular enough for a single SQL expression):

| oplog row | event |
|---|---|
| `op='i'`, `o`=doc | `operationType:"insert"`, `fullDocument:o`, `documentKey:{_id:o._id}` |
| `op='u'`, `o`=merged doc, `o2`={_id} | `operationType:"replace"` (our `'u'` is replacement-style — honest), `fullDocument:o`, `documentKey:o2` |
| `op='d'`, `o`={_id} | `operationType:"delete"`, `documentKey:o`, no `fullDocument` |
| all | `_id:{"_data":"<016x hex of seq>"}`, `clusterTime:{$timestamp:{t:ts_t,i:ts_i}}`, `ns:{db,coll}` (split stored `ns` at the **first** dot — db names cannot contain dots, collection names can) |

`fullDocumentBeforeChange` is **omitted** — `whenAvailable` permits that, and Meteor
falls back to diffing against its cached copy.

---

## 5. The work

Conventions: tick a box only when its verify step passes **on both server versions**
(10.11 and 11.8) unless marked otherwise. Every phase leaves `main` green:
`chimera/scripts/test.sh --server <v>` before and after. Unknown/unsupported inputs
raise `not_implemented` with a message naming this file — never a silent no-op (M2.4 rule).

### Phase 0 — Characterize + stopgap (no C++; half a day)

The goal is to know exactly what stock Meteor 3.5 does against today's chimera, and give
users an escape hatch before the feature lands.

- [ ] **CS0.1** Scaffold a second probe app on Meteor 3.5.1 next to the existing one
  (same layout as [chimera/tests/meteor/](chimera/tests/meteor/README.md); keep the
  3.3.1 todos app — it becomes the oplog-fallback regression). Point it at chimera
  (`run-meteor.sh` pattern) with **no** reactivity override, and record in this file
  what actually happens: does the availability check pass (expected: yes, via
  `setName`) and does `watch()`'s failure loop or fall back? Attach the chimera error
  log lines and the Meteor console output below in § Findings.
- [ ] **CS0.2** Verify the assumed stopgap works: same app with
  `METEOR_REACTIVITY_ORDER=oplog,polling` must behave exactly like the 3.3.1 app
  (oplog tailing, reactive todos, `mariadb` INSERT appears live).
- [ ] **CS0.3** Document the stopgap where users will look: a short "Meteor 3.5+" note in
  [README § Compatibility](README.md#compatibility) (change streams are *coming*, until
  then set the reactivity order) and in [chimera/tests/meteor/README.md](chimera/tests/meteor/README.md).
- [ ] **CS0.4** Confirm where Meteor's fence gets its target timestamp from (read
  [`mongo_common.js`](https://github.com/meteor/meteor/blob/devel/packages/mongo/mongo_common.js)
  — `fenceWriteTsKey`, `_csTargetTsByCollection`): confirm or refute "write replies must
  carry `operationTime`" (§2.8). Adjust Phase 4's scope in this file if refuted.

### Phase 1 — Pure pieces first (translator-style, unit-testable without a server)

Follow the M7.1 precedent: anything decidable without a server lives where ctest can
reach it. Model the tests on [test_oplog.cpp](chimera/tests/unit/test_oplog.cpp).

- [ ] **CS1.1** Resume-token codec: `seq (uint64) ⇄ {"_data": "<016x lowercase hex>"}`.
  Reject junk loudly (wrong type, odd length, non-hex) with the same error a driver
  would classify as non-resumable. Pure functions, new file pair
  `chimera/translator/{include/chimera,src}/changestream.{h,cpp}`.
- [ ] **CS1.2** `$changeStream` option validation as a pure function: BSON stage body in →
  parsed options out. Accept `fullDocument` (any of `default`, `updateLookup`,
  `whenAvailable`, `required` — we always have the post-image, so all are satisfiable),
  `fullDocumentBeforeChange` (`off`/`whenAvailable` accepted; `required` →
  `not_implemented`, we do not store pre-images), `startAfter`, `resumeAfter`,
  `startAtOperationTime`. **Reject** with `not_implemented`: `allChangesForCluster`
  (whole-cluster watch), database-level watch (`aggregate: 1`), `showExpandedEvents`.
  Exactly one resume origin may be present (server error `40674` shape: mutually
  exclusive) — test each pair.
- [ ] **CS1.3** Unit tests for both, including round-trips and the mutual-exclusion
  matrix. `ctest` green.

### Phase 2 — Open the stream (plugin: `aggregate` + registry)

- [ ] **CS2.1** New `chimera/plugin/chimera_mongo/changestream.{h,cc}` (added to
  [CMakeLists.txt](chimera/plugin/chimera_mongo/CMakeLists.txt)) holding: the
  change-event `CONCAT` SQL expression (sibling of `kEntryExpr`, per the mapping table
  in §4 — remember `JSON_QUOTE` for anything textual and the o2/absence rules), a
  `read_changestream(sql, ns, after_seq, limit)` that filters with plain
  `WHERE ns = ?` (no `compile_filter` — ns equality is the only filter a change stream
  ever needs), and seq resolution for the three start modes:
  - token → `after_seq = seq` (strictly after);
  - `startAtOperationTime (t,i)` → `after_seq = COALESCE(MAX(seq) WHERE ts_t < t OR (ts_t = t AND ts_i < i), 0)` (events at-or-after the time are delivered — matches server semantics);
  - none → `after_seq = oplog_head()` (start from "now").
- [ ] **CS2.2** Extend `TailState` ([cursor.h](chimera/plugin/chimera_mongo/cursor.h)) with a
  `change_stream` flag + target `ns` string (the BSON `filter` member stays for plain
  oplog tails). Keep the struct dumb; the registry still never holds a SQL session.
- [ ] **CS2.3** Intercept in [`cmd_aggregate`](chimera/plugin/chimera_mongo/commands.cc#L479):
  first stage `$changeStream` → validate via CS1.2 (any *additional* stage after it →
  `not_implemented`; Meteor never sends one), resolve `after_seq`, open the tail cursor,
  reply with an **empty `firstBatch`**, `cursor.id != 0`, `cursor.ns = "<db>.<coll>"`,
  plus `cursor.postBatchResumeToken` (token of `after_seq`) and top-level
  `operationTime` (current clock — see CS4.1's helper). `$changeStream` anywhere but
  first, or on `local.oplog.rs`, or under `$sql` → `not_implemented`.
- [ ] **CS2.4** Smoke test by hand with mongosh against a dev server:
  `db.parts.watch()` returns a live cursor; a wire insert in a second mongosh prints an
  `insert` event with correct `fullDocument`, token, `clusterTime`. (mongosh uses the
  same Node driver — this exercises the exact code path Meteor will.)

### Phase 3 — Drain it (`getMore`, PBRT, history-lost)

- [ ] **CS3.1** In [`cmd_get_more`](chimera/plugin/chimera_mongo/commands.cc#L400): when the
  cursor's `TailState.change_stream` is set, run the `tail_batch` loop against
  `read_changestream` instead of `read_oplog`. Reuse the head-skip trick verbatim
  (advance to head when the ns filter rejected everything — rejected rows must never be
  revisited) and the same park/wake (`wait_for_oplog_write`, `kOplogPollMs`, `maxTimeMS`
  deadline). Change-stream events are **always** delivered oldest-first; there is no
  `$natural: -1` here.
- [ ] **CS3.2** Reply shape: `nextBatch` of events, `cursor.id` unchanged,
  `cursor.postBatchResumeToken` = token of the post-batch `after_seq`, top-level
  `operationTime`. This is what lets the driver resume correctly across quiet periods.
- [ ] **CS3.3** History-lost: on open (CS2.3) *and* on every `getMore`, if
  `after_seq + 1 < MIN(seq)` in `chimera_meta.oplog` → error
  `{ok: 0, code: 286, codeName: "ChangeStreamHistoryLost", errmsg: …}`. Meteor is built
  to recover from exactly this (drops token, resyncs). Never silently skip the gap.
- [ ] **CS3.4** Make the 286 rule airtight against pruning: [`prune_oplog`](chimera/plugin/chimera_mongo/oplog.cc)
  must always leave **at least the newest row** (both the row-count and the age branch),
  so `MIN(seq)` exists whenever anything was ever written and the CS3.3 predicate is
  decidable. An empty-since-birth oplog receiving any token is also 286 (a token cannot
  legitimately exist). Extend the pruner knob docs accordingly.
- [ ] **CS3.5** `killCursors` already kills tail cursors via the registry — add a spec
  assertion, don't assume.

### Phase 4 — `operationTime` plumbing (the fence story)

Scope-check against CS0.4 findings first.

- [ ] **CS4.1** Helper in the plugin: current `(ts_t, ts_i)` read from
  `chimera_meta.oplog_clock` (one indexed-PK row; inside the caller's session). Used as
  `operationTime` in replies.
- [ ] **CS4.2** `ping` reply gains `operationTime` — Meteor uses it twice (start-time pin
  §2.2, caught-up floor §2.5-adjacent). Cheap, unconditional.
- [ ] **CS4.3** Write replies (`insert`, `update`, `delete`, `findAndModify` if/where it
  exists) gain `operationTime` read **inside the same transaction, after the
  statement** — the trigger has already stamped the clock row under its lock at that
  point, so this returns exactly *our* write's stamp (multi-row writes: the last stamp,
  which is what a fence wants). Verify with a unit-style SQL check in the differential
  spec: reply `operationTime` equals the `ts` of the write's own oplog row.
- [ ] **CS4.4** End-to-end fence check (deferred until Phase 6 app exists, listed here for
  scope): a Meteor method that writes and returns must not resolve on the client before
  the subscription shows the write. Meteor logs
  `change stream catching up took too long` when this plumbing is wrong — grep for it.

### Phase 5 — Differential + regression coverage

- [ ] **CS5.1** The reference mongod (8.0.12) must answer `$changeStream`, which requires a
  replica set: teach the differential harness ([chimera/tests/differential/](chimera/tests/differential/))
  to start the reference with `--replSet rs0` + `rs.initiate()` (one-node). Existing specs
  must stay byte-identical — this flips nothing else about the reference.
- [ ] **CS5.2** New spec `chimera/tests/differential/specs/changestreams.js` following the
  [cursors.js](chimera/tests/differential/specs/cursors.js) house style, covering at
  minimum: open with empty pipeline → empty `firstBatch` + nonzero id; insert/replace
  ('u')/delete event shapes (compare `operationType`, `documentKey`, `fullDocument` —
  **mask** `_id`/tokens, `clusterTime`, and `ns` differences the same way existing
  specs mask volatile fields); resume via `startAfter` picks up exactly-after; getMore
  on a quiet stream returns empty batch with a PBRT; `killCursors`; bogus token → error
  class; extra stage after `$changeStream` → error on chimera (reference differs here —
  fence it as a documented divergence, like other `not_implemented` fences).
- [ ] **CS5.3** Unit + differential + existing suites green on both versions
  (`chimera/scripts/test.sh --server 10.11` and `--server 11.8`), 8/8 + new spec.

### Phase 6 — Meteor 3.5 acceptance (the actual bar, mirrors M6)

- [ ] **CS6.1** The CS0.1 Meteor 3.5.1 todos app, default settings (change streams
  first): starts clean, and chimera's log shows a `$changeStream` aggregate per
  observed collection and **no** tail on `local.oplog.rs`.
- [ ] **CS6.2** Two-browser reactivity: a todo added in one appears in the other without
  refresh, served by the change-stream driver (assert
  `handle._multiplexer._observeDriver._usesChangeStreams` via a server-side probe like
  [probe-meteor.sh](chimera/tests/meteor/probe-meteor.sh), or accept the log evidence
  from CS6.1).
- [ ] **CS6.3** The party trick, again: `INSERT` via the **`mariadb` client** appears live
  in both browsers (trigger → oplog → change-stream cursor → DDP). This is the D7 story
  surviving the driver swap.
- [ ] **CS6.4** Fence check from CS4.4 passes (no catching-up warnings under normal use).
- [ ] **CS6.5** Regressions: the 3.3.1 app (oplog driver) and the 3.5.1 app with
  `METEOR_REACTIVITY_ORDER=oplog,polling` still pass M6.3/M6.4 behavior — we now serve
  *both* generations of Meteor reactivity.
- [ ] **CS6.6** Repeat CS6.1–CS6.3 on the 10.11 build.
- [ ] **CS6.7** Demo script `chimera/scripts/demo-changestream.sh --server <v>` in the
  house style of [demo-oplog.sh](chimera/scripts/demo-oplog.sh): shell A `watch()`es,
  shell B writes over the wire, shell C writes via `mariadb` — all events stream to A.

### Phase 7 — Docs & bookkeeping (same PR as the code they describe)

- [ ] **CS7.1** [README § Compatibility](README.md#compatibility): move change streams out
  of "Explicitly not supported" into the supported table with its honest subset
  (collection-level watch, empty pipeline, resume via `startAfter`/`startAtOperationTime`,
  history-lost signalling; **no** pre-images / `updateDescription` / database- or
  cluster-level watch / `showExpandedEvents` / `invalidate` events). Update the Meteor
  positioning line — oplog tailing *and* change streams, Meteor ≤3.4 and 3.5+.
- [ ] **CS7.2** Remove the CS0.3 stopgap notes (or demote to "only needed pre-vX").
- [ ] **CS7.3** [chimera/README.md](chimera/README.md) architecture blurb + this file's
  § Findings updated; tick the M5 sequel note in
  [chimeraDB-plan.md](chimeraDB-plan.md); add the feature to the release-plan packaging
  smoke test if v1 has not shipped by then.
- [ ] **CS7.4** Risks table in chimeraDB-plan.md gains: "Meteor 3.5 change-stream driver
  expectations beyond this plan → gap-list process, stub honestly" (mirror of the M6
  risk line).

---

## 6. Exit criteria (all must hold, both server versions)

- [ ] Stock Meteor 3.5.1 todos app is reactive against chimera with **zero configuration**, on the change-stream driver.
- [ ] A raw-SQL `INSERT` via the `mariadb` client appears live in the browser through a change stream.
- [ ] Meteor ≤3.4 / forced-oplog behavior unchanged (full existing suite green).
- [ ] A pruned-away resume token produces `ChangeStreamHistoryLost` (286) and Meteor visibly recovers (log line + UI converges).
- [ ] Differential spec green against the replica-set reference; divergences fenced and documented, never silent.

## 7. Non-goals (documented divergences — say so in the README, fail loudly in code)

- Database-level (`aggregate: 1`) and cluster-level (`allChangesForCluster`) watches.
- `updateDescription` / delta events (`showExpandedEvents`): our `'u'` is replacement-style; we emit `replace`, which every driver and Meteor handle.
- Pre-images (`fullDocumentBeforeChange: "required"`); `whenAvailable` is accepted and never satisfied.
- `invalidate`/`drop`/`rename` events: a dropped collection's stream simply goes quiet (triggers vanish with the table). Real MongoDB invalidates the cursor; Meteor ignores such events either way. Documented, not emulated.
- `$match`/`$project`/anything after `$changeStream` in the pipeline (Meteor never sends one; others get `not_implemented`).
- Multi-shard resume-token semantics — meaningless on a set of one.

## 8. Risks

| Risk | Mitigation |
|---|---|
| Node-driver resume assertions we haven't met (PBRT/operationTime edge cases) | CS2.4 mongosh smoke test uses the same driver; CS5.2 diffs against a real replica-set reference |
| Fence semantics subtly wrong → hung or premature methods | CS0.4 verifies the mechanism from Meteor source before Phase 4 is built; CS4.4/CS6.4 test it end-to-end; Meteor logs the failure mode by name |
| Pruner races a slow consumer → silent gap | CS3.3 makes the gap an explicit 286; CS3.4 keeps the predicate decidable |
| Reference-as-replica-set destabilizes existing differential specs | CS5.1 requires byte-identical existing transcripts before the new spec lands |
| Per-collection streams multiply tail cursors | Each is one registry entry + one indexed range scan per batch; the M8 performance baseline (roadmap item 1) will measure tail latency under write load either way |

## 9. Findings (fill in during Phase 0)

> *CS0.1 —* …
> *CS0.4 —* …
