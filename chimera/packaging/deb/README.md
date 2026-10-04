# Debian packages

Build from the repository checkout with Docker Desktop or another Docker engine:

```sh
./chimera/packaging/deb/build.sh --series 10.11 --arch amd64
./chimera/packaging/deb/test.sh --series 10.11 --arch amd64
```

Debian 12 (`--suite bookworm`, the default) supports both series;
`--series 11.8` selects its signed MariaDB.org repository.
Debian 13 (`--suite trixie`) supports 11.8 from Debian's signed native repository.
`--arch arm64` supports the Docker image on Apple Silicon; `--arch both` builds
both outputs. Native Debian Intel is the package deployment target.
Other Debian releases and Ubuntu are not claimed by these recipes. The existing
shipping Docker images continue to use Debian 12.

For a Debian 13 Intel host, build and test separately from the Debian 12 artifacts:

```sh
./chimera/packaging/deb/build.sh --suite trixie --series 11.8 --arch amd64
./chimera/packaging/deb/test.sh --suite trixie --series 11.8 --arch amd64
```

MariaDB 10.11 has no
[official Debian 13 repository](https://mirror.mariadb.org/repo/10.11/debian/dists/).
The scripts reject that combination before building or configuring APT. Use the
Debian 12 Docker image for 10.11 on a Debian 13 host; do not mix Debian 12 APT
sources into the host to force a native installation. Debian 13's supported
native server is [MariaDB 11.8](https://packages.debian.org/trixie/mariadb-server).

Debian 12 results land in `chimera/packaging/dist/debian/<series>/<arch>/`;
Debian 13 results use `chimera/packaging/dist/debian-trixie/<series>/<arch>/`.
An explicit `--output DIR` uses `DIR/<series>/<arch>/` for either suite; choose
separate directories when building multiple distributions. Each set contains the three
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

Install the files from one matching distribution/series/architecture directory
together. Check `Distribution` and `MariaDB package` in `build-info.txt` first:

```sh
sudo apt install ./chimeradb*.deb
sudo systemctl restart mariadb
sudo chimeradb setup
sudo chimeradb status
```

The Mongo listener has **no authentication** and defaults to
`127.0.0.1:27017`. SQL uses the installed MariaDB service's existing settings.
For 11.8 on Debian 12, first configure MariaDB's signed 11.8 repository using
the [official repository instructions](https://mariadb.org/download/?t=repo-config).
Debian 13 needs only its normal Debian repositories for 11.8. The build report
names the exact MariaDB package needed; artifacts from one Debian release are
not interchangeable with another. This project does not
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

For a real package-version upgrade, keep the previous artifacts and build the
new revision into a separate output directory:

```sh
./chimera/packaging/deb/build.sh --series 10.11 --arch amd64 --revision 2 \
  --output "$PWD/chimera/packaging/dist/debian-revision2"
./chimera/packaging/deb/test.sh --series 10.11 --arch amd64 \
  --packages "$PWD/chimera/packaging/dist/debian-revision2/10.11/amd64" \
  --previous-packages "$PWD/chimera/packaging/dist/debian/10.11/amd64"
```

`--previous-packages` verifies both sets' checksums and package metadata before
installation: they must target the selected distribution/series/architecture, each set must
contain one consistent version, and the current version must be newer. It
installs the previous packages, initializes a database, writes a document and
customizes the config, then installs the current packages. Assertions require
the installed version to increase, the plugin to remain active, and the config
and data to survive. The normal reinstall, removal and purge tests then run too.
A `0.1.0-1` → `0.1.0-2` result demonstrates a Debian packaging-revision upgrade;
it is not evidence of a cross-ChimeraDB-version data migration or a MariaDB
server-version upgrade.

For the same revision-upgrade test on Debian 13, use `--suite trixie --series 11.8`
on both commands and pass the corresponding Debian 13 directories. Pin
`--mariadb-version` to the previous build report's exact server package when
building revision 2 so the test changes only ChimeraDB's packaging revision.

## Native systemd acceptance

[`systemd-test.sh`](systemd-test.sh) exercises the host's real MariaDB service.
Run it as root only on a fresh, disposable Debian 12 or 13 test machine with
systemd running, `iproute2` installed, and working APT access. It requires the
explicit `--ack-disposable-host` flag and refuses existing MariaDB, MySQL or
Chimera packages, database paths/configuration, or listeners on ports 3306 and
27017. Stop any Docker deployment publishing those host ports first. Use a new
test machine for another run; the harness deliberately refuses its own retained
installation on a subsequent invocation.

Copy the checkout and a complete package directory to the test host. The
directory must match the host's Debian release, architecture and requested
MariaDB series. Before changing the host, the harness verifies the complete
checksum manifest, build report, package versions and exact MariaDB dependency.
For Debian 13 Intel:

```sh
sudo ./chimera/packaging/deb/systemd-test.sh --ack-disposable-host \
  --series 11.8 \
  --packages "$PWD/chimera/packaging/dist/debian-trixie/11.8/amd64"
```

For Debian 12, use the matching `dist/debian/<series>/amd64` directory and select
10.11 or 11.8. The harness configures the appropriate repository and installs
the packages and Python client libraries. It checks real `systemctl` startup,
idempotent setup, native Mongo/SQL drivers, SQL-triggered change streams, both
language gateways, restart persistence and customized-config preservation on
reinstall. It then removes and purges only Chimera packages, verifies that
MariaDB still starts and the data remains, and reinstalls the current Chimera
packages. It never deletes the database data directory or runs APT autoremove.

To include an actual package-version upgrade, provide the current package set
and an older one from the same distribution, series and architecture:

```sh
sudo ./chimera/packaging/deb/systemd-test.sh --ack-disposable-host \
  --series 11.8 \
  --packages /absolute/path/to/debian-trixie-revision2/11.8/amd64 \
  --previous-packages /absolute/path/to/debian-trixie-revision1/11.8/amd64
```

Both sets must depend on the same exact MariaDB server package; this tests a
Chimera package upgrade, not a MariaDB server migration. Without the previous
set, the harness explicitly reports that a version upgrade was not tested.

Success leaves the current Chimera packages installed and MariaDB enabled and
running. SQL root uses local Unix-socket authentication; Mongo remains on
`127.0.0.1:27017`. The `chimera_systemd_acceptance` database and systemd/APT logs
remain for inspection. Failure also preserves state and prints service/journal
diagnostics rather than attempting a destructive reset. The purge check removes
the test conffile customization; the final installation uses package defaults.

The native harness is available; a passing execution on the deployment host has
not yet been recorded. Container package tests do not establish systemd or LXC
deployment acceptance.

MongoDB is a trademark of MongoDB, Inc. MariaDB is a trademark of MariaDB plc.
ChimeraDB is an independent project and is not affiliated with, endorsed by, or
sponsored by MongoDB, Inc. or MariaDB plc. "MongoDB compatibility" describes
wire-protocol interoperability, not provenance.
