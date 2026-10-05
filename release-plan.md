# ChimeraDB Release Plan — packaging, distribution, and what isn't coming with it

**Date:** 2026-08-10 · **Last verified update:** 2026-10-04
**Continues:** [chimeraDB-plan.md](chimeraDB-plan.md) (M0–M7 done). This file owns **M9 —
Packaging & distribution**, and the tickets for three M8 items that are too large to be
backlog lines.

> **Doc map (DRY):** [README.md](README.md) owns the *what & why* and, critically, the
> install commands it already promises. [build-plan.md](build-plan.md) owns how the base
> MariaDB binaries were built. [chimeraDB-plan.md](chimeraDB-plan.md) owns the engineering
> milestones. This file owns **how the thing reaches a machine that is not this one** — and
> nothing else. Ground rules 1–5 of [chimeraDB-plan.md](chimeraDB-plan.md#non-negotiable-ground-rules)
> apply here unchanged; M9 adds no new ones.

---

## Delivery priorities — amended 2026-10-04

Execute M9 in the owner's requested order. Milestone numbers below identify work,
not delivery order; shared Debian package construction may happen before a Docker
image without making native Debian the first deliverable.

| Priority | Deployment | Acceptance gate |
|---|---|---|
| 1 | Docker on Apple Silicon Mac (`linux/arm64`) | Build image; fresh-volume initialization; SQL and Mongo clients; change streams; graceful stop; replacement container preserves data |
| 2 | Homebrew on macOS | Source-only tap install; dedicated service; setup; both protocols; restart; MariaDB upgrade/rebuild behavior |
| 3 | Docker in the supplied Debian 13 LXC on Intel Proxmox (`linux/amd64`) | Same image recipe and acceptance as Mac, run on the actual guest; SSH forwarding and volume persistence |
| 4 | Native Debian on Intel (`amd64`); supplied host is Debian 13 | Install matching local `.deb`s; systemd service; setup; client access; upgrade; remove/purge without losing data or preventing MariaDB startup |

Both MariaDB 10.11 and 11.8 remain required. Native Debian arm64 and Ubuntu
distribution testing are deferred; arm64 Debian packages are still an internal
input to Docker on the Mac. Cross-table projections and the M8 feature tickets
below remain outside this release effort. Homebrew's separate MariaDB keg is a
runtime dependency of that installation route, not a prerequisite for building
against the server copies already in this repository.

The supplied Intel target is a Debian 13 Proxmox **LXC**, replacing the originally
assumed Debian 12 VM for deployment acceptance. Debian 13 provides MariaDB 11.8;
10.11 has no official trixie repository and remains a Debian 12 Docker deployment
on this host. Debian 12 packages for both series remain supported by the existing
recipe and CI matrix; native Debian 12 systemd acceptance is a separate unverified
environment. Do not install Debian 12 native artifacts into Debian 13.

**Current evidence (2026-10-04):**

- The PR #8 summary-only findings are covered as well as the inline threads.
  Both full native suites now pass **118 unit cases**, live regressions and all
  nine differential specs per series. Retention discovery runs before the writer
  lock; a live regression proves a no-op scan completes while another connection
  holds that lock, and the previous plugin fails this negative control.
  `chimeradb status` and Docker health now require a valid Mongo ping with a
  two-second deadline, using a compiled helper included in both package routes.
  **17 real TCP/CLI cases**, a deterministic two-case address-fallback fixture and
  **18 CLI regressions** pass. Twelve committed Homebrew packaging tests cover
  reinstall cleanup/exit status and deriving the formula version from the
  checksummed source archive, even when the checkout is at a different version.
  These regressions run in CI. Local evidence is in
  `chimera/.run/review-pr8/native-summary-10.11.log`,
  `native-summary-11.8.log`, `pruner-scan-negative.log`, `health-tests.log`,
  `health-build.log`, `cli-summary-tests.log` and `homebrew-packaging-tests.log`.
- The rollback-gap review at `f660b25` passes **115 unit cases and both full native
  suites**, with all nine differential specs per series. History loss now uses
  durable records of actual deletions, committed atomically with pruning; row
  limits count committed events. An isolated live fixture verifies initial and
  interior trigger rollbacks, head/resume-zero/early-time streams, no-op and real
  pruning, restart persistence and conservative legacy migration. Migration
  refuses an active caller transaction promptly and preserves its rollback.
  The previous plugin fails the initial rollback-gap negative control. Evidence:
  `chimera/.run/review-pr8/native-watermark-10.11.log`,
  `native-watermark-11.8.log` and `rollback-negative-control.log`.
- The initial PR #8 review fixes at `f6659a3` pass **99 unit cases and the full native development suite
  on both MariaDB series**, including all nine differential specs. A stream opened
  at the oldest retained event's timestamp now replays that event; older pruned
  timestamps still report 286. The pruning regression waits for completed event
  reads from its specific pending getMore, and a negative control with the old
  entry-only history check fails after that synchronization.
- Repository trust now installs only the pinned MariaDB primary certificate,
  extracted from the upstream multi-key bundle and verified before APT sees it.
  Complete package-manifest equality and runtime package identities are checked
  before installation; debug symbols stay out of runtime image layers. All **35
  real GPG/Debian-archive regressions** pass, both arm64 runtime variants pass
  driver/persistence acceptance, and the updated 11.8 Debian lifecycle passes.
  The native systemd harness delegates to the same package validator, rechecks
  before each install and passes exactly three runtime packages to APT. Focused
  native-harness regressions reject symlinks, duplicate debug artifacts, invalid
  dependencies and incomplete manifests before any installation can occur.
- Homebrew retains an initialization marker until server readiness and catalog
  setup complete, and refuses partial or interrupted initialization. It rechecks
  the data after claiming initialization and checks the marker again when system
  tables already exist. All **nine recovery/concurrency regressions** and
  fresh-start/restart staging tests against both real Homebrew MariaDB kegs pass.
  Both new race tests fail against the previous service wrapper. These focused
  recovery and package-input checks run in the package CI workflow.
- The subsequent Docker initialization review also passes: marker acquisition is
  atomic, data is rechecked after the claim, and the existing-volume path rejects
  an initialization that began between its checks. Deterministic container tests
  cover simultaneous claims, partial-state retries, delayed contenders and newly
  appearing system tables. Both arm64 runtime variants pass again; the old
  touch-based entrypoint fails the simultaneous-claim negative control. These
  checks run as part of Docker acceptance in the package matrix.
- Debian package recipes, Docker runtime/Compose/smoke scripts, Homebrew formula
  generation/build/service/smoke scripts, and the Linux package CI workflow are
  implemented and verified as described below. Public release publication remains open.
- At code commit `5f78542`, the full native development suite passes against
  MariaDB 10.11.18 and 11.8.8: **93 unit cases, live regressions, SQL/wire demos
  and all nine MongoDB differential specs** on each. Local evidence is in
  `chimera/.run/release/native-10.11-isolated-clock.log` and
  `native-11.8-isolated-clock.log`. Fresh-start Homebrew staging also passes
  against both installed versioned kegs.
- Change-stream review fixes R1–R3, cold-start history loss and first-ping clock
  initialization are verified. A missing clock is bootstrapped in a separate
  `SqlSession`, so initialization DDL cannot commit a caller's SQL gateway
  transaction. Deterministic tests cover session identity and propagated errors.
- The CLI checks include failed readiness when the plugin is missing/inactive,
  a live Mongo ping, endpoint selection and Homebrew defaults-file forwarding.
- **All four Debian package and Docker combinations pass locally:** MariaDB
  10.11.18 and 11.8.9 × `arm64` and `amd64`. Package tests cover installation,
  repeated setup, conffile preservation on reinstall, restart, removal, purge and
  data preservation. Runtime tests cover wire CRUD, SQL visibility, SQL-to-change
  streams, oplog, `mongo()`, gateway isolation, host-loopback publication and data
  surviving replacement of the container. These ran through OrbStack on the Mac;
  the local `amd64` executions used emulation. The final
  [package CI run](https://github.com/mieweb/chimeraDB/actions/runs/37214386998)
  at `5f78542` passes all four combinations on native architecture runners,
  including the first-ping fix and package revision upgrades.
- Both Homebrew formulae pass source installation, `brew test --force`, fresh
  ping before any writes, dedicated launchd service operation, real Mongo/SQL
  drivers, reinstall, custom config and data persistence on clean macOS CI
  runners. The full recorded MariaDB keg identity includes package revision;
  the negative test rejects a changed revision before data initialization.
- Concrete **`0.1.0-1` → `0.1.0-2` Debian package revision upgrades pass** for
  both series and architectures in native CI and for both Intel packages locally
  under emulation. Tests assert the increased installed version, active plugin,
  custom config/data preservation, reinstall, removal and purge. This is a
  packaging revision upgrade within ChimeraDB 0.1.0, not a MariaDB server migration.
- The local Homebrew toolchain blocker is resolved. Both formulae now build and
  pass isolated runtime tests on this Mac against MariaDB 10.11.19 and 11.8.9,
  including fresh ping, gateways, oplog and restart persistence. The existing
  11.8 config is unchanged; both kegs remain unlinked and no global service was
  started. Reinstall can unexpectedly link a previously unlinked formula on this
  Homebrew version, so `local-test.sh` restores its prior state on exit; seven
  mocked success/failure cases verify that cleanup preserves the original error.
- Both Intel Docker images now pass on the supplied Debian 13 Proxmox LXC:
  full drivers, SQL-triggered change streams, graceful stop, replacement-container
  persistence and SSH-forwarded SQL/Mongo access from the Mac. Separate persistent
  Compose deployments are healthy with loopback-published ports. The same updated
  images pass on Mac arm64, and its preview was refreshed without replacing data.
- Actual Intel deployment exposed a Docker startup race: `chimeradb status` could
  report success against the temporary initialization server, which has no SQL TCP
  listener. The healthcheck now also rejects `.chimera-initializing`; runtime tests
  inject that marker and execute the image's real probe before driver acceptance.
- Debian 13/11.8 package builds now use trixie's signed native MariaDB source and
  exact server dependency, with separate output paths and a dedicated amd64 CI job.
  Debian source entries preserve the configured signing key, including the newer
  `.pgp` filename; guessing `.gpg` caused an APT conflict on the new base image.
  Native systemd acceptance passes against `1:11.8.6-0+deb13u1`, including a real
  `0.1.0-1` → `0.1.0-2` upgrade, real Mongo/SQL drivers, SQL-triggered change
  streams, restart, edited-config preservation on reinstall, remove/purge and
  retained data. The final `0.1.0-2` installation is enabled and running.
  Both revisions also pass the disposable Debian 13 package lifecycle test.
- The updated [package CI run](https://github.com/mieweb/chimeraDB/actions/runs/37242018652)
  at `d7bd90e` passes all **nine jobs**, including Debian 13; the separate
  [test workflow](https://github.com/mieweb/chimeraDB/actions/runs/37242018619)
  also passes. The original four Debian 12 CI package sets and both locally built
  Debian 13 revisions have verified checksums. All four updated Docker images pass
  runtime acceptance again; both Intel image archives and the source archive have
  verified checksums. Artifacts are in `chimera/packaging/dist/`. The refreshed Mac
  preview is healthy on SQL `127.0.0.1:13306` and Mongo `127.0.0.1:37017`, with its
  persistent volume retained.
  No public release assets, container manifests or Homebrew tap have been published.

**Network acceptance:** native packages bind the Mongo listener to loopback. The
Docker image explicitly opts into binding within its container, while the supplied
Compose file publishes only to host loopback. Verify both boundaries. Containers on
the same bridge remain able to reach the unauthenticated Mongo endpoint; use a private
network. This replaces the blanket container-loopback wording in the original plan.

Implementation entry points: [Docker](chimera/packaging/docker/README.md),
[Homebrew](chimera/packaging/homebrew/README.md),
[Debian](chimera/packaging/deb/README.md). Unchecked boxes below remain unchecked
until their verification gates pass.

---

## Why this milestone exists

The original README told a stranger to run (these promises are now removed from
[README § Installation](README.md#installation) until verified):

```sh
brew install chimeradb          # macOS
sudo apt install chimeradb      # Debian / Ubuntu
sudo dnf install chimeradb      # Fedora / RHEL
docker run -p 3306:3306 -p 27017:27017 chimeradb
chimeradb start
```

At the original plan baseline none of that existed. Every one of those lines was a promise the project had already made in
public, and the same README says ChimeraDB "never lies to a driver about features" — the
same standard should apply to its own front page. M9 either makes each line true or removes
it. **Scope decision below: brew and apt (+ Docker, nearly free) get built; `dnf` gets cut
from the README until someone wants it.**

At the original baseline everything in M0–M7 had been built and tested on exactly
one arm64 Mac, against MariaDB source trees inside this repo. M9 now has the Linux
package/runtime evidence above; clean-machine installation and distribution remain
separate gates from development-tree correctness.

---

## M9.0 — It has never been built on Linux *(do this first; everything else is downstream)*

At the original baseline, three concrete things were macOS-only:

| Evidence | Why it breaks on Linux |
|---|---|
| [build-plugin.sh](chimera/scripts/build-plugin.sh#L17), [build-translator.sh](chimera/scripts/build-translator.sh#L21) call `$(brew --prefix)` unconditionally | `brew: command not found` — both scripts die at line 1 of real work |
| [plugin CMakeLists](chimera/plugin/chimera_mongo/CMakeLists.txt) carries an `IF(APPLE)` dynamic-lookup link option for mysys symbols | The ELF path (symbols resolved from the `mariadbd` executable at load) is *assumed* to work and has never been observed to |
| [_common.sh](chimera/scripts/_common.sh) locates servers as `$REPO_ROOT/mariadb-10.11` and `$REPO_ROOT/mariadb-server` with a `dist/` prefix | A packaged install has no source tree and no `dist/` |

- [x] **M9.0.1** Make the pkg-config path discovery portable: use `pkg-config` as found, and
  only prepend a Homebrew prefix when `brew` exists. One change, both scripts, no new
  abstraction.
- [x] **M9.0.2** `chimera/packaging/docker/dev-debian.Dockerfile` — a Debian image with
  toolchain, `libbson-dev`, and a MariaDB source tree, that runs
  `chimera/scripts/test.sh --server <v>` unmodified. This proves the *existing* dev path on
  Linux before anything is repackaged.
  Built by [dev.sh](chimera/packaging/docker/dev.sh), which bind-mounts the checkout so the
  scripts under test are the working tree's. Two supporting decisions:
  `CHIMERA_OUT` relocates every build product to a container volume — one checkout cannot
  hold two platforms' cmake caches — and
  [build-server.sh](chimera/packaging/docker/build-server.sh) scripts the MariaDB build that
  [build-plan.md](build-plan.md) did by hand on macOS, with the storage engines ChimeraDB
  never loads switched off (1208 targets instead of ~4000).
- [x] **M9.0.3** Fix whatever M9.0.2 finds. Record each fix here as a correction note —
  divergences between the two platforms are the interesting output of this milestone.

  > **Correction 1 — `doctest` was an undeclared build dependency.** It reached the macOS
  > build through Homebrew and nothing said so. Debian's `doctest-dev` is now installed by
  > the image; on macOS nothing changes.
  >
  > **Correction 2 — libbson spells an empty document differently across major versions.**
  > 1.x (bookworm) emits `{  }`, 2.x (Homebrew) emits `{ }`, so three assertions in
  > [test_gateway.cpp](chimera/tests/unit/test_gateway.cpp) passed on one platform and failed
  > on the other. Normalized in the test, not in `to_extjson`: the spacing is libbson's to
  > choose, JSON whitespace is insignificant, and a structure-blind collapse applied to real
  > documents would corrupt string values that contain two spaces.
  >
  > **Correction 3 — the ELF link never worked, exactly as the table above suspected.**
  > MariaDB adds `-Wl,--no-undefined` to every non-storage-engine plugin on Linux
  > ([cmake/plugin.cmake:254](mariadb-10.11/cmake/plugin.cmake#L254)), on the assumption that
  > a plugin uses only the services ABI. ChimeraDB needs `current_thd`, `my_thread_init` and
  > `my_thread_end`, which live in the `mariadbd` executable, so the first Linux link ever
  > attempted failed with three undefined references. The flag is now dropped for this target
  > alone — the mirror image of the `IF(APPLE)` dynamic-lookup line, saying the same thing to
  > the other linker, and still with no server tree patched. It links, `mariadbd` loads it,
  > and the Mongo listener comes up on Linux.
  >
  > **Correction 4 — the reference is a macOS binary, and that is what blocks the exit
  > criteria.** Every layer that drives a mongo shell (`demo-m3`, `demo-oplog`,
  > `demo-projection`'s wire half, `demo-gateways`, the differential suite) fails in the
  > container with `Exec format error`. The shell is MongoDB 8.0.12 built from `mongodb/`,
  > and the legacy `mongo` client has not shipped in an official tarball since 5.0, so there
  > is nothing to download — a Linux reference means building MongoDB inside the image.
  > `mongosh` is not a substitute: the specs call `db.runCommand()` synchronously and mongosh
  > returns promises. **This remains necessary for M9.0's full development pyramid
  > and M9.6.1.** Packaged runtime acceptance now uses standalone Mongo and SQL
  > drivers and therefore does not depend on this reference shell.

  What *is* proven on Debian 12 / arm64 against MariaDB 10.11.18 built by
  [build-server.sh](chimera/packaging/docker/build-server.sh): hygiene, all 73 translator
  unit tests, the plugin link *and load*, `probe-json`, and the SQL halves of `demo-m1` and
  `demo-projection` — every D3, D8 and D10 assertion, identical to macOS.

- [ ] **M9.0.4** Run the complete development `test.sh` pyramid for both server series
  on arm64 **and** amd64. The server-independent suite and the packaged plugin,
  SQL/Mongo, change-stream and persistence paths now pass in the Linux package
  matrix on native CI runners. The Linux reference-dependent demos/differential
  suite have not yet run; local amd64 package/runtime validation used emulation.

**Exit criteria:** `test.sh` green for 10.11 and 11.8 inside a Debian container, on both
architectures, with no source changes made outside `chimera/`.

> **Status:** no upstream MariaDB source patches were needed. The full native macOS
> development suite passes on both series and all four Linux package/runtime
> combinations pass. The original full Linux development-suite exit criterion is
> still gated on the reference toolchain described in Correction 4.

---

## M9.1 — The structural decision: building a plugin with no server source tree

This is the crux of the whole milestone and needs a decision before any packaging code.

The development plugin is built through [link-plugin.sh](chimera/scripts/link-plugin.sh), which
symlinks `chimera/plugin/chimera_mongo/` into the server tree and the **server's** CMake
builds it via `MYSQL_ADD_PLUGIN` — which is precisely how ground rule 2 (zero upstream
patches) is honored. Packaging obtains the matching server source as a build input;
installed artifacts do not need that tree.

| | **A. Build the server source in the image** | **B. Standalone CMakeLists against installed server headers** |
|---|---|---|
| Fidelity | Identical to the dev path; ABI match guaranteed by construction | A second build path that can drift from the one developers use |
| Image / time | Whole MariaDB tree per series per arch | Minutes; small image |
| Cross-arch | **Impractical** — a full server build under qemu for the foreign arch is not a thing anyone will wait for | Cheap enough that qemu is tolerable, native runners better |
| Risk | Low technical risk, high friction | Must reproduce `MYSQL_ADD_PLUGIN`'s defines by hand (`MYSQL_DYNAMIC_PLUGIN`, and the `MYSQL_SERVER` exposure that [M7.2](chimeraDB-plan.md#milestone-7--cross-language-ergonomics) confined to `mongogateway_udf.cc`) |

This table records the original alternatives. D11 below supersedes its header
availability and full-server-build cost assumptions.

- [x] **M9.1.1** Spike: does Debian's `libmariadbd-dev` actually ship the server plugin
  headers (`mysql/plugin.h`, `mysql/service_sql.h` — M4.1 depends on the SQL service) for
  **both** 10.11 and the MariaDB.org 11.8 packages? Answer decides whether B is even
  available. Record the answer here either way.

  > **Yes, both.** `libmariadbd-dev 1:10.11.18-0+deb12u1` (bookworm) and
  > `libmariadbd-dev 1:11.8.8+maria~deb12` (deb.mariadb.org, arm64 present) each install
  > `/usr/include/mariadb/server/mysql/plugin.h` and `.../service_sql.h`. Note the path:
  > the *server* headers live under a `server/` subdirectory that the client headers do not
  > use, so an include path of `/usr/include/mariadb` alone finds the wrong ones.
- [x] **M9.1.2** Same question for Homebrew's MariaDB kegs (M9.3 needs it too — one spike,
  two consumers).

  > **Yes**, at `<keg>/include/mysql/server/mysql/{plugin,service_sql}.h`. The spike also
  > identifies M9.3.2's runtime dependencies: `mariadb@10.11` and `mariadb@11.8`
  > both exist. Shipped headers were subsequently found insufficient, so the
  > ChimeraDB formula also obtains the matching verified source tarball to build
  > the plugin target; it does not rebuild the server.
- [x] **M9.1.3** **Decision** (record it as a locked decision, D11): A, B, or B-with-A-as-CI-referee.
  Recommendation: **B**, keeping A as the developer path, plus a CI job that builds both and
  diffs the resulting module's undefined-symbol set. Without that referee, path B rots
  silently and the first person to notice is a user whose server won't start.

  > **D11 locked as A, restricted to the plugin target** — the recommendation did not
  > survive the attempt. See [chimeraDB-plan.md § Locked decisions](chimeraDB-plan.md#locked-decisions).
  >
  > **B is not available.** The headers are all there (M9.1.1), and ten of the eleven plugin
  > sources compiled cleanly against them. The eleventh does not:
  > [mongogateway_udf.cc](chimera/plugin/chimera_mongo/mongogateway_udf.cc) includes
  > `sql_class.h` to read `current_thd->db` — the database a SQL caller is already in, which
  > is what `mongo('db.parts.find(…)')` means by "here". `sql_class.h` is server-internal and
  > in no package, and `mysql/plugin.h` exposes nothing equivalent: it hands out
  > `thd_sql_command`, `thd_tx_isolation`, `thd_get_thread_id`, and no way to ask what schema
  > the session is in. `libmysqlservices.a` is missing too, but that one is a five-line shim.
  > (`chimera_mongo.cc`'s `sql_plugin.h` include *was* removable and has been removed — the
  > listener now lives in a file-scope pointer beside the pruner, so the module's exposure to
  > server internals is one file rather than two.)
  >
  > **A is far cheaper than this table claims.** "Whole MariaDB tree per series per arch"
  > conflated *configuring* the tree with *building the server*. Building only the
  > `chimera_mongo` target from an empty build directory pulls mysys, strings and `GenError`
  > — **222 targets, 19 seconds** on Debian arm64. The cross-arch objection goes with it.
  >
  > The referee is therefore deleted rather than built. Two build paths needed a diff to stay
  > honest; one build path is honest because it is one object.
- [x] **M9.1.4** Encode the ABI boundary in package names and dependencies. Plugin
  packages are named per series and require the **exact MariaDB package version**
  whose signed source was used at build time. The stricter patch-level dependency
  protects the SQL gateway's internal `THD` access. Both versions and architectures
  have installed and loaded successfully with these dependencies.

---

## M9.2 — Debian packages, built in Docker, for arm64 and amd64

- [x] **M9.2.1** [deb/build.sh](chimera/packaging/deb/build.sh)
  `--series 10.11|11.8 --arch amd64|arm64|both` builds with `docker buildx`
  and standard `debian/` metadata. Outputs go to
  `chimera/packaging/dist/debian/<series>/<arch>/`, with checksums and a build
  report recording the exact server package. CI uses the same script. All four
  local builds completed. A fresh export directory prevents stale package
  revisions being mixed into a later install.
- [x] **M9.2.2** Package split:

  | Package | Arch | Contents |
  |---|---|---|
  | `chimeradb-plugin-10.11` / `chimeradb-plugin-11.8` | any | `chimera_mongo.so` in `/usr/lib/mysql/plugin/` and the config drop-in |
  | `chimeradb-common` | all | `catalog.sql` in `/usr/share/chimeradb/sql/`, the CLI and man page |
  | `chimeradb` | all | Metapackage requiring common and the versioned virtual `chimeradb-plugin` provided by either series package |

  The two plugin packages conflict because they own the same module path.
  Debhelper also produces a plugin `-dbgsym` artifact. There are no separate
  `oplog.sql` or `triggers.tpl.sql` assets: the plugin creates that DDL itself.
  Install the files from one output directory with `apt install ./chimeradb*.deb`;
  bare `apt install chimeradb` requires a public repository that does not yet exist.
- [x] **M9.2.3** Ship `/etc/mysql/mariadb.conf.d/60-chimera.cnf` with
  `plugin-load-add=chimera_mongo`, experimental plugin maturity, port 27017 and
  **`loose-chimera-mongo-bind=127.0.0.1`**. This is the Mongo listener setting;
  MariaDB's SQL `bind-address` is a separate option. The `loose-` prefix lets the
  server restart after ordinary package removal leaves a conffile but removes
  the module. All four lifecycle tests assert loopback and successful restart
  after removal. Package descriptions and install documentation state that the
  Mongo listener has no authentication ([#5](https://github.com/mieweb/chimeraDB/issues/5)).
- [x] **M9.2.4** Maintainer scripts print setup/removal instructions and never
  connect to the database or run SQL. `chimeradb setup` loads `catalog.sql` and
  creates `mongo()`; repeated setup passes in all four package tests.

  > **Observed packaging correction:** MariaDB's own Debian package has a trigger
  > that may restart its service when plugin files change. ChimeraDB's maintainer
  > scripts do not restart it, but installation cannot promise no service restart.
  > The README, man page and install notice now explain this. Container tests and
  > image builds suppress service starts with `policy-rc.d`; native installs need
  > a maintenance window.
- [x] **M9.2.5** `chimeradb start` uses `systemctl start mariadb` followed by
  readiness/status checks on a systemd installation; it does not create another
  service manager. Missing/inactive plugin and incomplete catalog return failure.
  The Homebrew wrapper identifies its dedicated formula/service. Native Debian
  systemd execution is verified on Debian 13 in the acceptance below.
- [x] **M9.2.6** Native deployment acceptance on the supplied **Debian 13 Intel
  LXC with systemd**, using native MariaDB 11.8 and matching ChimeraDB artifacts.
  Debian 12 container coverage is complete for its 10.11 packages and MariaDB.org's
  11.8 packages, on both architectures. The new guarded `systemd-test.sh` passes
  actual service startup, both drivers/gateways, streams, restart, package revision
  upgrade, config-preserving reinstall and removal/purge without losing data.
  It leaves MariaDB 11.8.6 and ChimeraDB 0.1.0-2 enabled and running.
  Native Debian 12 systemd remains unverified.
  Ubuntu and native Debian arm64 remain deferred; arm64 packages support Mac Docker.
- [x] **M9.2.7** The plugin depends on the **exact** `mariadb-server` package version
  used to obtain its signed APT source; `dpkg-shlibdeps` derives runtime library
  dependencies, including bookworm's `libbson-1.0-0`. Clean installation and plugin
  load pass with `1:10.11.18-0+deb12u1` and `1:11.8.9+maria~deb12` on both architectures.
  This does not claim compatibility with another distro or untested MariaDB update.
- [x] **M9.2.8** Package descriptions carry the trademark statement from
  [README § License & trademarks](README.md#license--trademarks).

> **Build correction:** Debian's patched source declares a duplicate static client
> archive output that Ninja rejects. Package builds use CMake's Unix Makefiles
> generator for the server and still build only `chimera_mongo` (D11). The independent
> translator build uses Ninja. No upstream source edit was needed.

---

## M9.3 — Homebrew tap

- [ ] **M9.3.1** Publish `github.com/mieweb/homebrew-chimeradb` with verified
  formulae and source archive checksums, then advertise
  `brew tap mieweb/chimeradb && brew install chimeradb`. The private local test tap
  `chimera-local/release-validation` has exercised source installation in CI; it is
  not a public distribution. Bare `brew install chimeradb` is not promised today.
- [x] **M9.3.2** Depend explicitly on Homebrew's versioned server formulae:
  `chimeradb` uses `mariadb@11.8`, and `chimeradb@10.11` uses `mariadb@10.11`.
  The generator takes the exact dependency source URL/checksum from Homebrew,
  configures that server source and builds only the plugin target. Staging passes
  against both installed supported kegs, and both source formula installs succeeded
  on clean GitHub Actions macOS runners.
- [x] **M9.3.3** Complete clean-runner service and lifecycle acceptance. The module,
  CLI and SQL install into ChimeraDB's own keg; generated `etc/<formula>.cnf`,
  `var/<formula>` data and a dedicated launchd service avoid modifying another keg
  or global `my.cnf`. Both series pass clean CI source installation,
  `brew test --force`, first Mongo ping before writes, launchd start/stop/restart,
  real Mongo/SQL drivers, reinstall and custom config/data persistence. The
  first-ping clock initialization fix also passes fresh local staging on both.
  The keg-identity guard's negative revision-change test passes in CI: a changed
  MariaDB keg is refused before data initialization and requires a plugin rebuild.
  This verifies mismatch protection, not migration to a different MariaDB server.
- [x] **M9.3.4** Source-only formula installation is implemented and has succeeded
  for both series on clean CI runners. No bottles are published or required.
- [ ] **M9.3.5** Add tag-driven tap updates. The canonical template and renderer
  live in [homebrew/](chimera/packaging/homebrew/); a release must render the real
  source URL/checksum and update the separate tap repository. This automation is
  not implemented or published yet.

> **Homebrew toolchain status (2026-10-04):** the stale CLT installation was removed
> and Homebrew's minimum-toolchain check now passes with full Xcode 27 selected.
> Both formulae pass actual source installation and isolated runtime tests on this
> Mac against MariaDB 10.11.19 and 11.8.9. Clean CI also passes source installation,
> formula tests and the complete service/lifecycle checks for both series. The
> earlier staging-only limitation is resolved; publication of the tap remains open.

---

## M9.4 — Docker image *(first delivery; reuses M9.2 package construction)*

- [x] **M9.4.1** [runtime.Dockerfile](chimera/packaging/docker/runtime.Dockerfile)
  uses Debian 12 with the matching MariaDB and ChimeraDB packages. Its entrypoint
  initializes an empty persistent volume and runs setup; the container config
  explicitly enables its non-loopback listener. Images build and pass runtime
  acceptance for both series and architectures through OrbStack. The final native
  Linux CI matrix also passes all four combinations with the fresh-ping fix.
  Multi-architecture manifest publication is a separate, uncompleted gate.
- [ ] **M9.4.2** Publish the verified images and multi-architecture manifest under
  `mieweb/chimeradb`, then advertise that registry reference. Current artifacts are
  local `chimeradb:0.1.0-<series>-<arch>` images, not published pulls.
- [x] **M9.4.3** The packaged image passes the driver/SQL party trick, raw-SQL change
  stream, oplog, gateway-isolation, graceful-stop and replacement-container
  persistence tests for all four combinations. No source tree is present in the
  running image. Both images now also pass on the supplied Intel Proxmox LXC.

---

## M9.5 — Verifying artifacts from the outside

Development-tree tests cannot establish installation, service or upgrade behavior.
Package lifecycle and runtime-driver acceptance therefore have separate scripts.

- [x] **M9.5.1** [deb/test.sh](chimera/packaging/deb/test.sh) installs the artifacts
  into a clean disposable Debian container; [docker/test.sh](chimera/packaging/docker/test.sh)
  runs real Mongo/SQL drivers against the shipping image. All four combinations pass
  wire CRUD, SQL visibility, oplog, a raw-SQL write arriving through a change stream,
  `mongo()` and restart/replacement persistence. These tests use the packaged artifact
  without the development source tree or Linux reference `mongo` shell.
- [x] **M9.5.2** Complete deployment acceptance on the supplied Intel Proxmox LXC
  and native Debian 13 Intel/systemd. Clean Homebrew formula tests and
  launchd/lifecycle acceptance pass for both series. The Debian package/image
  matrix {10.11, 11.8} × {amd64, arm64} passes locally and in the final native
  Linux CI run, including the readiness fix; local amd64 used emulation.
  Actual Intel Docker acceptance and SSH forwarding now pass for both series;
  native Debian 13/11.8 systemd acceptance also passes. Ubuntu and native
  Debian arm64 are deferred; the originally assumed Debian 12 VM was not supplied.
- [x] **M9.5.3** **Package revision upgrade acceptance.** Install → upgrade →
  reinstall → remove → purge → MariaDB still starts and preserves data passes
  for all four combinations on native Linux CI runners. Both Intel package
  upgrades also pass locally under emulation.
  Debian 13/11.8 additionally passes this upgrade in both a disposable container
  and the actual Intel LXC's native systemd service.
  [deb/test.sh](chimera/packaging/deb/test.sh) now accepts `--previous-packages DIR`
  to install the older set, initialize/write/customize, upgrade to the newer set,
  assert a strictly increased installed package version plus active plugin and
  retained config/data, then run reinstall/removal/purge. Both inputs must match
  series and architecture. The verified upgrade is **`0.1.0-1` → `0.1.0-2`**:
  a packaging revision change within the first ChimeraDB version, 0.1.0. It does
  not claim migration across ChimeraDB versions or MariaDB server versions.
  Retained config after removal uses `loose-` plugin options; the missing module logs
  an error but does not prevent MariaDB startup, as verified in the lifecycle tests.
- [x] **M9.5.4** Runtime assertions confirm native-package Mongo loopback defaults
  in all four package tests and host-loopback publication in all four Docker tests.
  Homebrew staging and clean CI also assert loopback on both series. The image deliberately
  binds inside its container; containers sharing its bridge are within the trust boundary.

---

## M9.6 — CI and release automation

[.github/workflows/test.yml](.github/workflows/test.yml) covers server-independent
hygiene and translator/storage tests. [package.yml](.github/workflows/package.yml)
builds and tests the Linux artifact matrix and installs/tests both Homebrew formulae.
At code commit `5f78542`, all eight jobs in
[package run 37214386998](https://github.com/mieweb/chimeraDB/actions/runs/37214386998)
passed and the run succeeded; the separate
[test run 37214387046](https://github.com/mieweb/chimeraDB/actions/runs/37214387046)
succeeded. Tag-driven publication remains a separate gate.

- [ ] **M9.6.1** Extend `test.yml` to the full M0–M7 development pyramid for both
  series on Linux. The existing native amd64/arm64 hygiene and unit jobs do not
  replace the reference-dependent layers blocked by M9.0's Correction 4.
- [x] **M9.6.2** Obtain a green corrected `package.yml` test matrix: Debian builds,
  lifecycle tests and runtime-driver acceptance on native Linux runners, plus clean
  Homebrew install, formula tests and service/lifecycle acceptance for both series.
  The final run above passes all four Linux combinations, including package
  revision upgrade, remove/purge and Docker wire/stream/persistence checks.
  Both Homebrew source installs, formula tests, fresh ping, launchd lifecycle,
  stale-keg rejection, real drivers and reinstall/config/data persistence pass.
  The subsequent `d7bd90e` run passes all nine jobs, adding Debian 13/11.8 amd64
  installation and package revision upgrade to the matrix.
- [ ] **M9.6.3** Implement `release.yml`: on tag, publish `.deb`s and corresponding
  source to GitHub Releases, push the Docker manifest, and update the public tap.
  None of these public release steps has run. Plain downloadable packages come
  first; an APT repository and signing-key management remain a separate decision.
- [x] **M9.6.4** Use native CI runners and thin script wrappers. Both Linux workflows
  select `ubuntu-24.04-arm` for arm64 and `ubuntu-24.04` for amd64;
  `package.yml` uses `macos-15` for Homebrew. Scripts are the shared local/CI entry
  points. The complete test matrix has passed under M9.6.2.
- [x] **M9.6.5** [check-hygiene.sh](chimera/scripts/check-hygiene.sh) is wired into
  `test.yml` to enforce the SSPL source boundary.

---

## M9.7 — Version identity

- [x] **M9.7.1** There is no version number anywhere in `chimera/`. Add `chimera/VERSION`
  (start at `0.1.0`) as the single source for package versions, formula, and image tags.
- [x] **M9.7.2** Decide what ChimeraDB reports to a driver in `buildInfo` — it is what
  `mongosh` prints on connect. It must not claim to be a MongoDB version it is not, and it
  must not be so strange that drivers refuse it. This is the same "never lie in the
  handshake" rule the M6 stubs followed, applied to the string humans actually see.

  > **`6.0.0-chimera-<VERSION>`.** It was a bare `6.0.0`, which is the one thing the rule
  > forbids. The prefix is the wire version actually advertised (`maxWireVersion` 17), so a
  > driver gating on it still gets a true answer; the suffix is valid semver prerelease, so
  > version parsers accept it and it sorts *below* 6.0.0 rather than above — the safe
  > direction to be wrong in. `gitVersion` follows as `chimera-<VERSION>`. Verified against
  > both shells: `mongosh` prints `Using MongoDB: 6.0.0-chimera-0.1.0`. The current
  > differential suite passes all nine specs on both server series.
- [x] **M9.7.3** Encode product and server identity without conflating them:
  `chimeradb-plugin-11.8_0.1.0-1_amd64.deb` carries series in its package name,
  product version/revision in its version, and an exact MariaDB package dependency.
  `build-info.txt` records that dependency; common/metapackage versions stay shared
  across series. Docker tags include product version, series and architecture.

---

## M9 exit criteria

- [x] On clean Debian containers (both arches) and clean macOS CI runners: install,
  `setup`, and run the SQL/Mongo party trick using installed artifacts without a
  development source tree at runtime. Both server series pass; Homebrew also passes
  its dedicated launchd lifecycle. Actual Intel Proxmox LXC Docker and native
  Debian 13 systemd acceptance also pass under M9.5.2.
- [ ] Every install command printed in [README.md](README.md#installation)
  either works verbatim or has been removed. `dnf` is removed unless someone builds it.
- [ ] Artifacts are produced by CI from a tag, not by a human.
- [x] Native-package Mongo listeners and Docker host port publications are loopback-bound in
  every shipped default until authentication exists
  ([#5](https://github.com/mieweb/chimeraDB/issues/5)); container binds are the explicit
  exception described in the delivery priorities above. Verified in all four Linux
  package/image combinations and both clean CI Homebrew installations.

---

# Tickets — three M8 items that are their own projects

These are spun out of [chimeraDB-plan.md § Milestone 8](chimeraDB-plan.md#milestone-8--hardening-backlog-explicitly-out-of-scope-for-v1--do-not-start-without-discussion).
Each is filed as a GitHub issue. None is scheduled; each needs its own decision before it
starts, and the decision — not the implementation — is what the issue leads with.

---

### T1 — Vector search (plugin-side MHNSW) · [#2](https://github.com/mieweb/chimeraDB/issues/2)

**Summary.** `$vectorSearch`-shaped stage plus `createIndexes {type:"vectorSearch"}`, backed
by a per-index sibling InnoDB graph table maintained by the translator through the SQL-service
choke point, in the same transaction as wire writes. Raw-SQL writes reconciled asynchronously
by tailing the M5 oplog. No server `VECTOR` type, parser, or optimizer support required, so
10.11 works identically to 11.8.

**Why it is not in the release.** It is plausibly larger than M1–M7 combined — a graph index,
its transactional maintenance, an async reconciler, and a query surface — and it is the only
part of the system with **nothing to diff against**, because community mongod has no
`$vectorSearch`. Golden files would be the weakest evidence in the project, in its most
intricate component. M6 demonstrated what golden-ish testing misses.

**Decisions needed before any code:**
1. Is this v1.x of ChimeraDB, or a separate project that depends on it? It has its own
   testing story, its own performance characteristics, and its own audience.
2. **11.8 has a native `VECTOR` type. Do we use it when present?** Using it violates rule 4's
   "identical on both versions"; ignoring it means deliberately shipping a slower path on the
   newer LTS. The current backlog line assumes the second without arguing for it.
3. What is the correctness bar with nothing to diff against? Recall parity against a
   reference implementation? A brute-force exact search as the referee for small datasets —
   which is cheap and would be genuinely convincing?

**Notes.** Porting the neighbor-selection heuristic from
[mariadb-server/sql/vector_mhnsw.cc](mariadb-server/sql/vector_mhnsw.cc) is license-compatible
(GPLv2 → GPLv2; ground rule 1 quarantines `mongodb/` only, not MariaDB). Known costs to size
up front: SQL round-trips on cold search (a plugin-side graph cache mitigates) and serialized
inserts at the entry-point row.

**Definition of done.** A stage that answers correctly on both LTS versions, a stated recall
target with evidence, and a documented consistency model for raw-SQL writes.

---

### T2 — `chimerash`, the dual-language REPL · [#3](https://github.com/mieweb/chimeraDB/issues/3)

**Summary.** A client-side router: naked SQL goes out over the MySQL protocol, naked mongosh
syntax goes out over the Mongo wire protocol, at one prompt. This is the no-server-fork answer
to the thing ground rule 2 forbids, and the promise in
[README § One prompt, both languages](README.md#one-prompt-both-languages).

**Why it is not in the release.** It is not a server feature at all — it is a new binary with
its own language choice, build, packaging, install path, and release cadence. Nothing else in
this repo is a client.

**Decisions needed:**
1. **Does it overlap `mongo('…')` to the point of redundancy?** M7.2 already lets a SQL client
   run any supported mongosh statement. `chimerash` removes the quotes. Is removing the quotes
   worth a new component — or is it the demo that makes the whole thesis land in ten seconds,
   in which case it is marketing-critical and should be scheduled deliberately rather than
   inherited from a backlog?
2. Language and dependency budget. Anything that drags a Node or Python runtime into the
   install story affects M9's packaging directly.
3. Own repo, or `chimera/shell/` here? If here, it becomes another package in M9.2's split.
4. Dispatch rule: how does it decide which language a line is, and what happens when the guess
   is wrong? A wrong guess that silently runs the other engine is worse than a parse error.

**Definition of done.** Both languages at one prompt against one connection pair, packaged and
installed by the same M9 pipeline, with a documented and testable dispatch rule.

---

### T3 — `eager` projection automation · [#4](https://github.com/mieweb/chimeraDB/issues/4)

**Summary.** [D4](chimeraDB-plan.md#locked-decisions) promised three projection modes;
`manual` ships, `lazy` is trivial, `eager` means the server samples incoming documents for new
paths and issues `ALTER TABLE … ADD COLUMN … AS (JSON_VALUE(…)) PERSISTENT` on its own.

**Why it is not in the release.** The engineering question is downstream of a product question
nobody has answered: **do we want the database issuing DDL by itself?** An auto-`ALTER` on a
large collection is a table rebuild triggered by an insert that mentioned a new field. That is
a surprising amount of authority to hand a heuristic, and "manual + lazy, documented" may
simply be the honest product. D4 promised the knob; nothing forces the knob to be automatic.

**Decisions needed:**
1. Automatic DDL: yes or no. If no, D4 is amended and the item closes — a legitimate outcome.
2. If yes: what is the sampling policy (path frequency, type stability, minimum document
   count), and who runs it — a background thread, or a `chimera_suggest_projections()` that
   proposes ALTERs for a human to run? The second gets most of the value with none of the
   authority, and is far smaller.
3. Behavior under concurrent writes, and the failure mode when the ALTER cannot complete.

**Definition of done.** Either an amended D4 with the reasoning recorded, or a policy with
measured rebuild cost on a realistic collection and a documented way to turn it off.
