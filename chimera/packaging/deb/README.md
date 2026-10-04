# Debian 12 packages

Build from the repository checkout with Docker Desktop or another Docker engine:

```sh
./chimera/packaging/deb/build.sh --series 10.11 --arch amd64
./chimera/packaging/deb/test.sh --series 10.11 --arch amd64
```

`--series 11.8` selects the signed MariaDB.org repository for Debian 12.
`--arch arm64` supports the Docker image on Apple Silicon; `--arch both` builds
both outputs. Native Debian Intel is the initial package deployment target.
Other Debian releases and Ubuntu are not claimed by these recipes.

Results land in `chimera/packaging/dist/debian/<series>/<arch>/`: the three
application packages, an automatically generated plugin debug-symbol `.deb`,
`build-info.txt` and `SHA256SUMS`. Builds use the repository's ChimeraDB
version, a numeric `--revision` (default 1), and `SOURCE_DATE_EPOCH` (default:
last Git commit time). `--mariadb-version` pins an APT package version for a
repeat build; by default the configured repository's current candidate is
selected and recorded. Repository snapshots, base image digests and toolchain
versions are not frozen, so this is a repeatable recipe, not a claim of
bit-for-bit reproducibility across time.

The builder downloads the exact selected MariaDB package's signed APT source
and configures its CMake, then builds only the `chimera_mongo` target (D11).
The server uses Unix Makefiles because Debian's patched source declares a
duplicate static client archive output that Ninja rejects. The independent
translator build continues to use Ninja; no upstream source edits are needed.
It runs the translator unit suite and lets `dpkg-shlibdeps` derive native
library dependencies. No full MariaDB server build is needed.

Install the files from one series/architecture directory together:

```sh
sudo apt install ./chimeradb*.deb
sudo systemctl restart mariadb
sudo chimeradb setup
sudo chimeradb status
```

The Mongo listener has **no authentication** and defaults to
`127.0.0.1:27017`. SQL uses the installed MariaDB service's existing settings.
For 11.8, first configure MariaDB's signed 11.8 Debian 12 repository using
the [official repository instructions](https://mariadb.org/download/?t=repo-config).
The build report names the exact MariaDB package needed. This project does not
yet provide a public APT repository; `apt install chimeradb` alone will not find
these local packages.

`chimeradb-common` contains the CLI, `catalog.sql` and its manual.
`chimeradb-plugin-10.11` and `chimeradb-plugin-11.8` cannot coexist: they share
one plugin path. Both provide `chimeradb-plugin`, which the `chimeradb`
metapackage requires. The plugin package depends on the **exact MariaDB package
version** used at build time, because the SQL gateway uses internal THD layout.
Build a new package revision for each MariaDB package upgrade and install the
two updates together. A series-only dependency would silently allow an
unverified binary combination.

ChimeraDB's maintainer scripts never start, stop or contact MariaDB. However,
MariaDB's own Debian package triggers may restart its service when plugin
files are installed or upgraded. Install during a maintenance window, then run
the printed restart/setup commands. Re-running setup is safe. Removing or
purging ChimeraDB never deletes schemas, collection data,
oplog history, the catalog or the SQL function registration. Restart MariaDB
after removal to unload the running plugin. The SQL `mongo()` function cannot
be used without the plugin library; reinstalling restores it.

`test.sh` uses a disposable container and a fresh data directory. It verifies
install, repeated setup, an SQL-to-Mongo gateway write, conffile preservation
on reinstall, data persistence on restart, remove, and purge. Reinstall checks
the package lifecycle; it does not claim a migration test between two different
ChimeraDB releases. Wire-protocol acceptance is covered by the runtime image
smoke test.

MongoDB is a trademark of MongoDB, Inc. MariaDB is a trademark of MariaDB plc.
ChimeraDB is an independent project and is not affiliated with, endorsed by, or
sponsored by MongoDB, Inc. or MariaDB plc. "MongoDB compatibility" describes
wire-protocol interoperability, not provenance.
