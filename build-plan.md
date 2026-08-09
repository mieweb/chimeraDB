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
- [ ] **A4 Build** — background job, ~20–45 min on Apple Silicon
  ```sh
  cmake --build build -j <half-of-cores>
  ```
- [ ] **A5 Verify**
  ```sh
  build/sql/mariadbd --version
  build/client/mariadb --version
  ```

**Exit criteria:** `mariadbd` and `mariadb` client binaries report the pinned version.

### Track B: MongoDB (Agent B)

> **Correction (2026-08-09):** the pinned tag `r8.0.28` builds with **Bazel**, not
> SCons — `buildscripts/scons.py` no longer exists in this checkout, and there is no
> `etc/pip/compile-requirements.txt`. The original plan's SCons assumption was wrong
> for this patch release. Steps below reflect the actual `docs/building.md` flow.

- [x] **B1 Clone** ✅ `mongodb/` cloned at `r8.0.28`
- [x] **B2 Dependencies** ✅ `llvm@19`/`lld@19` via brew (Bazel toolchain, not a Python venv — no compile-requirements.txt in this version)
- [x] **B3 Configure check** ✅ bazelisk installed → `~/.local/bin/bazel`; resolved to `bazel 7.5.0-mongo_9ea3a8ad9f`
- [ ] **B4 Build** — background job, ~1–3 hrs (the long pole)
  ```sh
  bazel build install-mongod --disable_warnings_as_errors=True
  ```
- [ ] **B5 Verify**
  ```sh
  bazel-bin/install/bin/mongod --version
  ```

**Exit criteria:** `mongod` binary reports the pinned version.

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
