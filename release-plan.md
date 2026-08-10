# ChimeraDB Release Plan — packaging, distribution, and what isn't coming with it

**Date:** 2026-08-10
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

## Why this milestone exists

[README § TL;DR](README.md#tldr--get-started-in-60-seconds) already tells a stranger to run:

```sh
brew install chimeradb          # macOS
sudo apt install chimeradb      # Debian / Ubuntu
sudo dnf install chimeradb      # Fedora / RHEL
docker run -p 3306:3306 -p 27017:27017 chimeradb
chimeradb start
```

None of that exists. Every one of those lines is a promise the project has already made in
public, and the same README says ChimeraDB "never lies to a driver about features" — the
same standard should apply to its own front page. M9 either makes each line true or removes
it. **Scope decision below: brew and apt (+ Docker, nearly free) get built; `dnf` gets cut
from the README until someone wants it.**

There is also a harder reason. Everything in M0–M7 was built and tested on exactly one
machine: an arm64 Mac, against MariaDB source trees that live inside this repo. The project
has never been compiled on Linux, never been built by anything but a human at this desk, and
never been installed anywhere. Until that changes, "it works" means "it works here."

---

## M9.0 — It has never been built on Linux *(do this first; everything else is downstream)*

Not a formality. Three concrete things are macOS-only today:

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
  > **Correction 4 — the test oracle is a macOS binary, and that is what blocks the exit
  > criteria.** Every layer that drives a mongo shell (`demo-m3`, `demo-oplog`,
  > `demo-projection`'s wire half, `demo-gateways`, the differential suite) fails in the
  > container with `Exec format error`. The shell is MongoDB 8.0.12 built from `mongodb/`,
  > and the legacy `mongo` client has not shipped in an official tarball since 5.0, so there
  > is nothing to download — a Linux oracle means building MongoDB inside the image.
  > `mongosh` is not a substitute: the specs call `db.runCommand()` synchronously and mongosh
  > returns promises. **This is the remaining work in M9.0 and a hard prerequisite for
  > M9.6.1.**

  What *is* proven on Debian 12 / arm64 against MariaDB 10.11.18 built by
  [build-server.sh](chimera/packaging/docker/build-server.sh): hygiene, all 73 translator
  unit tests, the plugin link *and load*, `probe-json`, and the SQL halves of `demo-m1` and
  `demo-projection` — every D3, D8 and D10 assertion, identical to macOS.

- [ ] **M9.0.4** Same on arm64 **and** amd64 (see M9.1 on why the arch matters this early).
  arm64 done as far as Correction 4 allows; amd64 not attempted.

**Exit criteria:** `test.sh` green for 10.11 and 11.8 inside a Debian container, on both
architectures, with no source changes made outside `chimera/`.

> **Status:** the last clause holds — every fix above landed inside `chimera/`. The rest is
> gated on a Linux oracle build (Correction 4), after which 11.8 and amd64 are only compute.

---

## M9.1 — The structural decision: building a plugin with no server source tree

This is the crux of the whole milestone and needs a decision before any packaging code.

Today the plugin is not built by us at all. [link-plugin.sh](chimera/scripts/link-plugin.sh)
symlinks `chimera/plugin/chimera_mongo/` into the server tree and the **server's** CMake
builds it via `MYSQL_ADD_PLUGIN` — which is precisely how ground rule 2 (zero upstream
patches) is honored. A package build has no server tree.

| | **A. Build the server source in the image** | **B. Standalone CMakeLists against installed server headers** |
|---|---|---|
| Fidelity | Identical to the dev path; ABI match guaranteed by construction | A second build path that can drift from the one developers use |
| Image / time | Whole MariaDB tree per series per arch | Minutes; small image |
| Cross-arch | **Impractical** — a full server build under qemu for the foreign arch is not a thing anyone will wait for | Cheap enough that qemu is tolerable, native runners better |
| Risk | Low technical risk, high friction | Must reproduce `MYSQL_ADD_PLUGIN`'s defines by hand (`MYSQL_DYNAMIC_PLUGIN`, and the `MYSQL_SERVER` exposure that [M7.2](chimeraDB-plan.md#milestone-7--cross-language-ergonomics) confined to `mongogateway_udf.cc`) |

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
  > answers M9.3.2 for free: `mariadb@10.11` and `mariadb@11.8` both exist as formulae, so
  > the tap never has to fetch its own server tarball.
- [x] **M9.1.3** **Decision** (record it as a locked decision, D11): A, B, or B-with-A-as-CI-referee.
  Recommendation: **B**, keeping A as the developer path, plus a CI job that builds both and
  diffs the resulting module's undefined-symbol set. Without that referee, path B rots
  silently and the first person to notice is a user whose server won't start.

  > **D11 locked as recommended** — see [chimeraDB-plan.md § Locked decisions](chimeraDB-plan.md#locked-decisions).
  > M9.1.1/M9.1.2 removed the only thing that could have forced A: the headers are there on
  > every target. The referee is what makes B safe to ship, so it is not optional — it is
  > M9.6.2's job and blocks the first release, not a nice-to-have.
- [ ] **M9.1.4** Whatever is chosen, the plugin ABI is tied to a server series. Packages are
  per-series; there is no "works on any MariaDB" artifact. Encode that in names and
  dependencies (M9.2), not in a README caveat.

---

## M9.2 — Debian packages, built in Docker, for arm64 and amd64

- [ ] **M9.2.1** `chimera/packaging/deb/` with the standard `debian/` metadata and
  `chimera/packaging/deb/build.sh --series 10.11|11.8 --arch amd64|arm64|both`, using
  `docker buildx`. Outputs to `chimera/packaging/dist/`. Script-first (ground rule 3): CI
  runs the identical command.
- [ ] **M9.2.2** Package split — the ABI split from M9.1.4 makes a single `chimeradb`
  binary package impossible:
  | Package | Arch | Contents |
  |---|---|---|
  | `chimeradb-plugin-10.11` / `chimeradb-plugin-11.8` | any | `chimera_mongo.so` → `/usr/lib/mysql/plugin/` |
  | `chimeradb-common` | all | SQL assets (`catalog.sql`, `oplog.sql`, `triggers.tpl.sql`) → `/usr/share/chimeradb/sql/`, the `chimeradb` CLI, man page |
  | `chimeradb` | all | Meta-package depending on `chimeradb-common` + the plugin matching the installed server — so `apt install chimeradb` from the README still works |
- [ ] **M9.2.3** **Config drop-in** `/etc/mysql/mariadb.conf.d/60-chimera.cnf`:
  `plugin_load_add=chimera_mongo`, `chimera_mongo_port=27017`, and **`bind-address=127.0.0.1`
  for the Mongo listener**. There is no authentication on that listener yet
  ([#5](https://github.com/mieweb/chimeraDB/issues/5)), so a package that binds it to
  `0.0.0.0` ships an unauthenticated database to the network. Localhost-only is not a default
  to be polite about — it is the security control, and it must be impossible to get by
  accident. The package description and `README` must say so in the same breath as the
  install command.
- [ ] **M9.2.4** **No clever maintainer scripts.** `postinst` cannot load SQL into a server
  that may not be running, may be remote, may need credentials. Ship `chimeradb setup`
  (loads the catalog/oplog SQL, creates the `mongo()` function) and have `postinst` print how
  to run it. A failed `postinst` leaves apt in a broken state; a printed instruction does not.
- [ ] **M9.2.5** Decide what `chimeradb start` means on a systemd box, because the README
  promises it. Honest options: (a) a thin wrapper over `systemctl start mariadb` that then
  verifies the plugin loaded and prints both endpoints; (b) drop `start` on Debian and make
  the CLI `setup`/`status`/`verify` only. Do **not** invent a second service manager
  alongside systemd. README changes either way.
- [ ] **M9.2.6** Target matrix, and the honest reason for each: Debian 12 (bookworm, native
  10.11), Ubuntu 24.04 (native 10.11), and MariaDB.org's 11.8 repo on both. Anything not in
  the matrix is not claimed.
- [ ] **M9.2.7** Dependencies: `mariadb-server` pinned to the matching series, plus libbson —
  which is `libbson-1.0-0` on bookworm but `bson2` upstream, exactly the split the
  [translator CMakeLists](chimera/translator/CMakeLists.txt#L12-L16) already handles. Verify
  the *runtime* dependency is expressed correctly for each distro, not just the build one.
- [ ] **M9.2.8** Package description carries the trademark statement from
  [README § License & trademarks](README.md#license--trademarks) verbatim.

---

## M9.3 — Homebrew tap

- [ ] **M9.3.1** **The README's `brew install chimeradb` cannot work as written.** A bare
  formula name means homebrew-core, which will not take a formula that builds against a
  keg-only versioned server, and shouldn't be asked to before the project has users. A tap
  must live in a repo literally named `homebrew-*`, so it cannot live in this repo either.
  Decision: create `github.com/mieweb/homebrew-chimeradb`, and change the README to
  `brew tap mieweb/chimeradb && brew install chimeradb`. Two lines instead of one, and true.
- [ ] **M9.3.2** Which MariaDB does it build against? Homebrew's `mariadb` tracks latest,
  which ChimeraDB does not support. Confirm which versioned formulae exist
  (`mariadb@10.11`, `mariadb@11.8`) and depend on those explicitly. If neither exists, the
  formula must fetch a matching source tarball itself — slow at install time but correct,
  and it makes the M9.1 decision moot on macOS.
- [ ] **M9.3.3** Where does the `.so` go? Writing into another formula's keg gets erased on
  its next upgrade. Install into ChimeraDB's own prefix and have `caveats` print the exact
  `plugin_dir`/`plugin_load_add` lines for `$(brew --prefix)/etc/my.cnf`. Verify the
  advertised lines by pasting them into a clean machine — caveats that were never executed
  are the most reliably wrong text in any formula.
- [ ] **M9.3.4** Source-only formula first (KISS). Bottles are a later optimization and they
  invalidate on every MariaDB keg bump, which is a maintenance treadmill nobody has signed
  up for yet.
- [ ] **M9.3.5** Release automation updates the tap on tag (formula source of truth lives in
  the tap repo; this repo pushes the bump).

---

## M9.4 — Docker image *(nearly free once M9.2 exists)*

- [ ] **M9.4.1** `FROM mariadb:11.8` + install the `.deb` + the config drop-in + entrypoint
  that runs `chimeradb setup` on first boot against its own server. Multi-arch manifest via
  the same buildx invocation as M9.2.
- [ ] **M9.4.2** Publish as `mieweb/chimeradb`, and fix the README's bare `chimeradb` image
  name to match.
- [ ] **M9.4.3** The image doubles as the package smoke test's happy path — it is the
  cheapest way for a stranger to reach the party trick in
  [README § The party tricks](README.md#the-party-tricks).

---

## M9.5 — Verifying artifacts from the outside

`test.sh` tests a build tree. It cannot tell you whether a package installs, and M6 already
taught this project that the gap between "the harness passes" and "a real client works" is
where the bugs live.

- [ ] **M9.5.1** `chimera/packaging/tests/smoke.sh` — against a **clean container with no
  source tree**: install, `chimeradb setup`, insert via a driver over the wire, read the same
  row via SQL, watch it arrive in the oplog, call `mongo('db.c.findOne({})')` from the SQL
  prompt. That subset covers M4, M5 and M7 through the packaged artifact only.
- [ ] **M9.5.2** Matrix: {bookworm, ubuntu 24.04} × {10.11, 11.8} × {amd64, arm64}.
- [ ] **M9.5.3** **Upgrade and purge are release-blocking tests.** A leftover
  `plugin_load_add=chimera_mongo` in a conf.d file after the `.so` is gone means `mariadbd`
  refuses to start — the package would break the user's database by being removed. Test
  install → upgrade → purge → server still starts.
- [ ] **M9.5.4** Verify the listener is bound to loopback in the shipped default (M9.2.3),
  as an assertion, not a code review.

---

## M9.6 — CI, because there is none

[.github/](.github) contains only `copilot-instructions.md`.
[test.sh](chimera/scripts/test.sh#L2) says "CI runs it twice (10.11, 11.8)"; nothing does.
Packaging without CI means release artifacts built by hand on one Mac.

- [ ] **M9.6.1** `test.yml`: the M0–M7 pyramid, both series, on Linux (uses M9.0's image).
- [ ] **M9.6.2** `package.yml`: builds every artifact in M9.2–M9.4 and runs M9.5 on each.
- [ ] **M9.6.3** `release.yml`: on tag, publishes `.deb`s to GitHub Releases, pushes the
  Docker manifest, bumps the tap. Plain files on a release first; an APT repository with
  signing is a separate decision with key-management consequences, and `apt install` from a
  downloaded `.deb` is honest in the meantime.
- [ ] **M9.6.4** Native arm64 runners where available rather than qemu; every workflow stays
  a thin wrapper over `chimera/packaging/*.sh` so a failure is reproducible locally.
- [ ] **M9.6.5** Wire in [check-hygiene.sh](chimera/scripts/check-hygiene.sh) — the SSPL
  grep of ground rule 1 is a CI check that has never run in CI.

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
  > both shells: `mongosh` prints `Using MongoDB: 6.0.0-chimera-0.1.0`, and the differential
  > suite is unchanged at 8/8.
- [ ] **M9.7.3** Package version encodes both: `0.1.0-mariadb11.8`.

---

## M9 exit criteria

- [ ] On a clean Debian container (both arches) and a clean Mac: install, `setup`, and run
  the README's party trick with no source tree present.
- [ ] Every install command printed in [README.md](README.md#tldr--get-started-in-60-seconds)
  either works verbatim or has been removed. `dnf` is removed unless someone builds it.
- [ ] Artifacts are produced by CI from a tag, not by a human.
- [ ] The Mongo listener is loopback-bound in every shipped default until authentication
  exists ([#5](https://github.com/mieweb/chimeraDB/issues/5)).

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
part of the system with **no differential oracle**, because community mongod has no
`$vectorSearch`. Golden files would be the weakest evidence in the project, in its most
intricate component. M6 demonstrated what golden-ish testing misses.

**Decisions needed before any code:**
1. Is this v1.x of ChimeraDB, or a separate project that depends on it? It has its own
   testing story, its own performance characteristics, and its own audience.
2. **11.8 has a native `VECTOR` type. Do we use it when present?** Using it violates rule 4's
   "identical on both versions"; ignoring it means deliberately shipping a slower path on the
   newer LTS. The current backlog line assumes the second without arguing for it.
3. What is the correctness bar with no oracle? Recall parity against a reference
   implementation? A brute-force exact search as the referee for small datasets — which is
   cheap and would be genuinely convincing?

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
