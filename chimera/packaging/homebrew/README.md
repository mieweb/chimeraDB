# Homebrew packaging

`chimeradb` uses Homebrew's `mariadb@11.8` server keg; `chimeradb@10.11`
uses `mariadb@10.11`. Only one ChimeraDB formula can be linked at a time.
The formula does **not** rebuild MariaDB. It downloads the dependency's exact
source version/checksum, configures the server CMake and builds only its
`chimera_mongo` target (D11). Source-only installation is intentional: no bottles.

The plugin, CLI and SQL live in ChimeraDB's own keg. Its launchd service owns a
separate data directory and config, keeping the normal MariaDB service independent.
It initializes the catalog automatically and defaults both protocols to loopback.
The defaults are SQL `3306` and Mongo `27017`; change the dedicated config if those
ports are already occupied. Never start both services on the same ports.
The service initializes its dedicated `var/chimeradb` (or `var/chimeradb@10.11`)
directory. Keep `datadir` at that generated path; changing only the config cannot
relocate initialization. Ports and listener settings can be customized.

## Prepare the tap

The intended tap is `mieweb/homebrew-chimeradb`. Until it is published, do not
advertise `brew tap mieweb/chimeradb` as an available installation.

After producing a versioned source archive containing `chimera/`, render both
formulae with its real SHA-256:

```sh
chimera/packaging/source.sh --ref <tested-commit-or-tag>
python3 chimera/packaging/homebrew/render-formula.py \
  --url https://github.com/mieweb/chimeraDB/releases/download/v0.1.0/chimeradb-0.1.0.tar.gz \
  --sha256 <archive-sha256> --output /path/to/homebrew-chimeradb/Formula
```

`source.sh` archives committed files only and writes the archive, SHA-256 and
commit metadata under `chimera/packaging/dist/source/`. Use that archive's
checksum and eventual published URL when generating release formulae.

The generator reads `chimera/VERSION`; the output belongs in the tap repository.
Do not publish placeholder checksums. The source URL and checksum must be updated
together for each release. The MariaDB source resource uses the trusted URL and
checksum from the corresponding Homebrew formula at build time, and `build.sh`
rejects a mismatch against the installed server.

Once the tap is published and formula tests pass:

```sh
brew tap mieweb/chimeradb
brew install chimeradb
brew services start chimeradb
chimeradb status
brew test chimeradb
```

For 10.11 substitute `chimeradb@10.11` in the install, services and test commands.
After MariaDB upgrades, run `brew reinstall chimeradb` and restart the ChimeraDB
service. The service refuses to load a plugin built for a different MariaDB patch
version or resolved keg, including a Homebrew revision bump with the same upstream
version. Stop the service before uninstalling. Configuration and data are preserved;
no plugin directive is inserted into global `my.cnf` or another formula's config.

## Development validation without installing another server

Existing supported MariaDB source builds can validate the identical build and
service scripts in an isolated prefix:

```sh
chimera/packaging/homebrew/build.sh --server 11.8 \
  --mariadb-source "$PWD/mariadb-server" \
  --mariadb-prefix "$PWD/mariadb-server/dist" \
  --prefix /tmp/chimeradb-stage-11.8 --build-dir /tmp/chimeradb-build-11.8
chimera/packaging/homebrew/smoke.sh --server 11.8 \
  --mariadb-prefix "$PWD/mariadb-server/dist" \
  --prefix /tmp/chimeradb-stage-11.8 --work-dir /tmp/chimeradb-smoke-11.8
```

Use `mariadb-10.11` and distinct directories for `--server 10.11`. The smoke test
starts a private server, checks setup, loopback, SQL↔Mongo gateway, oplog and
real Mongo C driver writes, restart persistence, and shuts down on completion. This validates packaging
staging; it does **not** replace testing `brew install`, `brew services` and
upgrades against real versioned Homebrew kegs.

For the complete formula installation test against the current working tree:

```sh
chimera/packaging/homebrew/local-test.sh --server 11.8
chimera/packaging/homebrew/local-test.sh --server 10.11
```

This creates the private local tap `chimera-local/release-validation`, generates
formulae with a checksum of the current sources, trusts those two generated
formulae when Homebrew requires it, installs each formula without
changing command links, and runs `brew test` with isolated state. It does not
publish a tap or start a global service. Add `--reinstall` after source changes
to rebuild an already installed test formula.

If Homebrew's host toolchain checks prevent a formula build but a current full
Xcode is installed, `stage-with-keg.sh --server 11.8` (or `10.11`) builds and tests
the same scripts against an installed versioned MariaDB keg and its verified
source archive. This gives useful runtime coverage while the host's Command
Line Tools are repaired, without claiming that `brew install` passed.
