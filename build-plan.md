# Build Plan: MariaDB & MongoDB from Source on macOS

**Date:** 2026-08-08
**Workspace:** `/Volumes/Case/prj/chimeraSQL`
**Goal:** Clone and build release/optimized native macOS binaries for MariaDB and MongoDB, executed as two parallel tracks.

## Parameters

| Parameter | Value |
|---|---|
| Versions | Latest stable release tags (MariaDB `mariadb-11.8.x` LTS, MongoDB `r8.0.x`) |
| Clone depth | Shallow (`--depth 1`) |
| Build type | Release/optimized (`RelWithDebInfo` for MariaDB, `--release` for MongoDB) |
| Layout | `mariadb-server/` and `mongodb/` under the workspace root |

## Concurrency model

Two agents each perform clone + dependency setup + configure, then launch their long
compile as a **background terminal job**. Both compiles run simultaneously; the main
agent monitors completion, handles failures, and verifies binaries.

---

## Phase 0 — Shared preflight (~5 min)

- [x] **M0.1 Toolchain & resources**
  - Xcode CLT: `/Applications/Xcode.app/Contents/Developer` ✓
  - Homebrew 6.0.15 ✓
  - Disk: 1.2Ti free of 1.8Ti — ample ✓
  - CPU count: 14 cores → 7 per track
- [x] **M0.2 Resolve latest stable tags (no clone needed)**
  - MariaDB → **`mariadb-11.8.8`**
  - MongoDB → **`r8.0.28`**

**Exit criteria:** toolchain present, tags pinned, enough disk. ✅ Done 2026-08-09.

---

## Phase 1 — Parallel tracks

### Track A: MariaDB (Agent A)

- [x] **A1 Clone** ✅ `mariadb-server/` cloned at `mariadb-11.8.8`
- [x] **A2 Dependencies** ✅ cmake, ninja, openssl@3, bison, gnutls, fmt, pcre2 installed via brew
  Note: macOS system bison is too old — prepend `$(brew --prefix bison)/bin` to `PATH`.
- [x] **A3 Configure** (out-of-tree; skip heavy optional engines) ✅ CMake+Ninja configure succeeded; OpenSSL, CURL, Curses found
- [x] **A4 Build** ✅ completed 2026-08-09, 2012/2012 targets, background job (`-j7`)
  ```sh
  cmake --build build -j <half-of-cores>
  ```
- [x] **A5 Verify** ✅ `mariadbd Ver 11.8.8-MariaDB for osx10.21 on arm64`; `mariadb from 11.8.8-MariaDB, client 15.2`
  ```sh
  build/sql/mariadbd --version
  build/client/mariadb --version
  ```

**Track A status: COMPLETE ✅ (2026-08-09)**

**Exit criteria:** `mariadbd` and `mariadb` client binaries report the pinned version.

### Track B: MongoDB (Agent B)

> **Correction (2026-08-09):** the originally pinned `r8.0.28` requires MongoDB's
> private/enterprise module even for a plain community build — `src/BUILD.bazel`
> unconditionally references `//src/mongo/db/modules/enterprise/...` labels with
> no `select()` fallback, and that directory doesn't exist in the public clone.
> Bisected the 8.0.x tag range: **SCons was removed in favor of Bazel between
> `r8.0.15` (SCons, last one) and `r8.0.18` (Bazel-only, same enterprise-module
> blocker)**. `r8.0.15` itself turned out to be a *partial* migration state —
> `src/third_party/murmurhash3/SConscript` (and likely others) had already been
> deleted in favor of `BUILD.bazel` while the top-level `SConstruct` still
> requires it. Bisected again: **`src/third_party/*/SConscript` files are fully
> intact through `r8.0.12`, first missing (murmurhash3) at `r8.0.13`.**
> Final retarget: **`r8.0.12`** — spot-checked 11 other third_party SConscripts
> (wiredtiger, mozjs, boost, benchmark, fmt, pcre2, s2, zlib, snappy, tomcrypt,
> gperftools) all present.

- [x] **B1 Clone** ✅ retargeted to `r8.0.12` (checked out in existing `mongodb/` repo)
- [x] **B2 Dependencies** ✅ `python@3.10` venv + `poetry==2.0.0` + `poetry install --no-root --sync`
  (had to pin `pip==23.3.2` in the venv — newer pip's PEP 517 legacy-build shim
  errors with `KeyError: 'PEP517_BUILD_BACKEND'` against the old `zope-interface==5.0.0` sdist).
  Vendored `src/third_party/scons-4.9.1/` isn't checked into git at all — installed
  PyPI `scons==4.9.1` into the venv and invoke `scons` directly instead of
  `buildscripts/scons.py` (which only looks for the vendored copy).
- [x] **B3 Configure check** ✅ `scons --dry-run` confirms clang/Xcode toolchain detection works
- [~] **B4 Build** — background job, ~1–3 hrs (the long pole); launched 2026-08-09, in progress
  ```sh
  scons install-mongod --disable-warnings-as-errors -j7
  ```
  Note: background job got SIGTTIN-suspended once when stdin was inherited from the
  terminal — relaunched with `< /dev/null` and `python3 -u` to stay detached and unbuffered.

  > **Correction (2026-08-09):** Build failed ~1/3 through with a genuine compile error
  > in vendored `src/third_party/boost/boost/thread/future.hpp:4672`:
  > `error: no member named 'that' in 'run_it<FutureExecutorContinuationSharedState>'`.
  > Inspecting the surrounding move-assignment operator showed every other member access
  > in the same struct uses `that_` (the real member name) — this one instance is a
  > plain upstream typo in this vendored Boost snapshot that newer/stricter clang
  > (Xcode 26.3) no longer tolerates. Fixed with a one-line patch (`x.that` → `x.that_`)
  > directly in the vendored header, then relaunched the same `scons install-mongod`
  > command (incremental — resumed past the fixed object file without a clean rebuild).
- [ ] **B5 Verify**
  ```sh
  build/install/bin/mongod --version
  ```

**Exit criteria:** `mongod` binary reports the pinned version (now `r8.0.15`).

---

## Phase 2 — Monitoring & failure handling

1. Agent A completes A1–A3, launches A4 in background, returns.
2. Agent B completes B1–B3, launches B4 in background, returns.
3. Main agent gets notified as each background build finishes.
4. On failure: capture error tail, fix root cause, resume — both Ninja and SCons
   builds are incremental, so no restart from scratch.

Common failure modes:

| Symptom | Likely fix |
|---|---|
| MariaDB cmake can't find SSL | Set `-DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)` |
| MariaDB bison version error | Ensure brew bison precedes `/usr/bin` in PATH |
| MongoDB warnings-as-errors | Re-run with `--disable-warnings-as-errors` |
| MongoDB Python version error | Use Python 3.10 venv, not system Python |
| Disk pressure mid-build | MongoDB build dir can exceed 30 GB — free space, resume |

---

## Phase 3 — Wrap-up

- [ ] Report binary paths, versions, and sizes for both builds.
- [ ] Optional smoke test:
  - MariaDB: initialize temp datadir, start `mariadbd`, connect with client, shut down.
  - MongoDB: start `mongod --dbpath <tmp>`, connect, shut down.

## Risks

- Running both builds at full `-j` saturates the machine — split cores (~half each).
- MongoDB 8.0 SCons vs. newest Xcode may need warning suppression or minor patches.
- Shallow clones can't switch tags later without `git fetch --deepen`/re-fetch.
