# Replace "oracle" terminology with "reference"

**Date:** 2026-08-10
**Status:** done — executed 2026-08-10, verification below all green
**Why:** "test oracle" is standard differential-testing vocabulary, but this is a MariaDB
project — a fork that exists *because* Oracle Corp bought MySQL — so "oracle mongod" reads
as "Oracle's mongod" to exactly the audience this repo addresses. "Reference" (as in
*reference implementation*) carries the same meaning with zero collision;
[release-plan.md](release-plan.md#L272) already uses it naturally.

## Vocabulary

| Current | Replacement |
|---|---|
| test oracle / the oracle (prose, comments) | reference / the reference mongod |
| `ORACLE_MONGO`, `ORACLE_MONGOD`, `ORACLE_MONGO_PORT` | `REFERENCE_MONGO`, `REFERENCE_MONGOD`, `REFERENCE_MONGO_PORT` |
| `ORACLE_DATA`, `ORACLE_LOG`, `oracle_pid`, `stop_oracle` | `REFERENCE_DATA`, `REFERENCE_LOG`, `reference_pid`, `stop_reference` |
| `$RUN_DIR/oracle/` runtime dir | `$RUN_DIR/reference/` |
| `<spec>.oracle.raw` / `<spec>.oracle` transcripts | `<spec>.reference.raw` / `<spec>.reference` |

**One definition survives** (DRY): ground rule 1 in
[chimeraDB-plan.md](chimeraDB-plan.md#L57) becomes "…a *black-box* reference (the "test
oracle", in differential-testing terms) and client binary only." Every other occurrence
just says *reference*.

## Inventory (first-party occurrences as of 2026-08-10)

- [x] **Shared script vars** — the single source everything else inherits:
  - [chimera/scripts/_common.sh](chimera/scripts/_common.sh#L73-L75) — the three `ORACLE_*` variables
- [x] **Consumers of those vars** (mechanical rename + local comments):
  - [chimera/tests/differential/run.sh](chimera/tests/differential/run.sh) — heaviest: vars,
    locals, `stop_oracle` trap, `$RUN_DIR/oracle/` paths, `.oracle[.raw]` transcript
    suffixes, header + inline prose (~20 spots)
  - [chimera/scripts/demo-m3.sh](chimera/scripts/demo-m3.sh) (4) ·
    [demo-oplog.sh](chimera/scripts/demo-oplog.sh) (1) ·
    [demo-projection.sh](chimera/scripts/demo-projection.sh) (1) ·
    [demo-gateways.sh](chimera/scripts/demo-gateways.sh) (1)
  - [chimera/tests/meteor/probe-meteor.sh](chimera/tests/meteor/probe-meteor.sh#L54) (1)
  - [chimera/scripts/check-hygiene.sh](chimera/scripts/check-hygiene.sh#L2) (2, comments only)
- [x] **Docs:**
  - [chimeraDB-plan.md](chimeraDB-plan.md) — 13 spots: doc-map intro (L18), ground rule 1
    (L57, keeps the parenthetical), repo-layout comment (L81), port table (L102), M0
    as-built table (L118), M0.5.2 (L190), M3.7 (L309), M4 correction notes (L358, L375),
    M4.3 (L368), M4.4 (L372), M8 line (L640), testing-strategy table (L666)
  - [release-plan.md](release-plan.md#L262) — 2 spots; **hand-edit, not sed**: L272 already
    says "against a reference", so the sentence must be reworded to avoid
    "reference vs reference" (e.g. "correctness bar with nothing to diff against")
  - [chimera/README.md](chimera/README.md#L5) — 2 spots (L5, L57)
  - [changestream-plan.md](changestream-plan.md) — 5 spots, written after this plan was
    drafted: CS5.1 (3), CS5.2, exit criteria, two risk-table rows
- [x] **Code comments** (no behavior change):
  - [chimera/plugin/chimera_mongo/collection.cc](chimera/plugin/chimera_mongo/collection.cc#L98) (L98, L253)
  - [chimera/plugin/chimera_mongo/commands.cc](chimera/plugin/chimera_mongo/commands.cc#L863)
  - [chimera/translator/src/update.cpp](chimera/translator/src/update.cpp#L140)
- [x] **Housekeeping:** delete stale `chimera/.run/oracle/` (gitignored runtime artifact;
  regenerated under the new name on next differential run — the "oracle" strings inside
  its mongod.log are just the old path echoed back)

## Explicitly out of scope

- `chimera-lite/wasm-poc/vendor/**` — third-party Go code; its Oracle mentions are the
  actual corporation (MySQL flavors, OCI cloud constants). Never edit vendor code.
- `chimera/.run/**` other than the directory deletion above (Meteor npm artifacts mention
  `vnd.oracle.resource+json` etc.)
- `mariadb-server/`, `mariadb-10.11/`, `mongodb/` — ground rule 2.

## Execution notes

- Case matters: sweep `oracle`, `Oracle` (prose at sentence start), `ORACLE_` — but only in
  the files listed; no blanket repo-wide sed.
- All within one commit: comments, prose, variable names, two runtime path names — zero
  functional change, so docs and scripts must not drift apart between commits.

## Verification

1. `grep -rni oracle *.md chimera/ --exclude-dir=.run` → exactly one hit: the ground-rule-1
   parenthetical. ✅
2. `chimera/tests/differential/run.sh --server 10.11` and `--server 11.8` green (rule 4) —
   proves the transcript-suffix and path renames hold together. ✅ 8/8 each
3. `chimera/scripts/check-hygiene.sh` still green. ✅ (plus `demo-gateways.sh --server 11.8`,
   to exercise a `REFERENCE_MONGO` consumer outside the differential harness)

One wording change beyond the table: check-hygiene.sh's "the one permitted kind of
reference" became "…kind of dependency", for the same collision reason as release-plan.md's.

## Commit

```
Docs/scripts: rename test-oracle terminology to "reference"

"Test oracle" is differential-testing jargon, but in a MariaDB project the
word reads as the company that bought MySQL. "Reference" says the same
thing without the collision. One parenthetical in ground rule 1 keeps the
jargon anchor for readers who know the literature. No functional change:
variable names, transcript suffixes, one runtime directory, prose.
```
