# Runnable Docker image

This is the release image recipe for Docker on an Apple Silicon Mac and
`linux/amd64` in a Linux VM on Intel Proxmox. It installs the same Debian packages
as a native installation. It is separate from `dev-debian.Dockerfile` (compiler
environment) and `build-wcdb-chimera.sh` (seeded WebChart development database).

**Validation status:** both server-series images build and pass the full runtime
smoke test on Apple Silicon using OrbStack. Intel builds and native VM validation
are tracked in the [release plan](../../../release-plan.md). No release image has
been published.

## Build, test and run on a Mac

From the repository root with OrbStack (or another Docker engine) running:

```sh
export DOCKER_CONTEXT=orbstack
./chimera/packaging/docker/build.sh --server 10.11 --arch arm64
export CHIMERA_IMAGE="chimeradb:$(cat chimera/VERSION)-10.11-arm64"
./chimera/packaging/docker/test.sh --image "$CHIMERA_IMAGE"
read -s -p 'New SQL root password: ' MARIADB_ROOT_PASSWORD; echo
export MARIADB_ROOT_PASSWORD
docker compose -f chimera/packaging/docker/compose.yaml up -d --wait
```

The password prompt above uses Bash (`bash` first if your shell is zsh).
Use your engine's context instead of `orbstack` when running elsewhere.
Use `--server 11.8` and a matching image tag for that series. Build both series
before calling a release verified. `--packages DIR` reuses previously built
packages from `chimera/packaging/dist/debian/<series>/<arch>/`.

Connect with `mongosh mongodb://127.0.0.1:27017/appdb` and
`mariadb -h127.0.0.1 -P3306 -uroot -p`. For local administrative SQL inside the
container, use `docker compose -f chimera/packaging/docker/compose.yaml exec db mariadb`.
The local root account authenticates through the Unix socket; the password
initializes `root@'%'` for TCP connections. Changing the environment variable
later does not reset an existing volume's password. `_FILE` is supported by the
entrypoint for deployments that provide a mounted secret.

The entrypoint initializes an empty volume once, runs `chimeradb setup`, then
runs MariaDB as `mysql`. The volume survives `docker compose down`; `down -v`
deletes it. Back up data before upgrading, and do not switch MariaDB series on an
existing volume without a separately tested migration. Replacing a container
with the same series must preserve data; `test.sh` checks this using a fresh
throwaway volume, plus graceful shutdown, driver CRUD, SQL visibility, oplog,
SQL-triggered change streams and the `mongo()` SQL function.

## Intel Proxmox

Use a Debian Linux **VM** on Proxmox with Docker Engine and Compose installed.
The hypervisor itself is not the installation target, and LXC nesting is not a
tested target. Run the same build/test commands inside that VM with `--arch amd64`
and `CHIMERA_IMAGE="chimeradb:$(cat chimera/VERSION)-10.11-amd64"`. Native amd64
validation in this VM is required; emulation on a Mac alone does not complete it.

To move an already built Intel image from the Mac without rebuilding on the VM:

```sh
./chimera/packaging/docker/export.sh --server 10.11 --arch amd64
```

Copy the resulting `.tar.gz` and `.sha256` from `chimera/packaging/dist/images/`
to the VM. Verify with `sha256sum -c <image>.tar.gz.sha256`, then
`docker load -i <image>.tar.gz`. Use that image tag with the same Compose file.
These archives contain the image only; they do not contain any database volume
or the local runtime password.

Compose publishes SQL and Mongo on the VM's loopback. Reach them from a Mac
using SSH forwarding, substituting the real VM host:

```sh
ssh -N -L 3306:127.0.0.1:3306 -L 27017:127.0.0.1:27017 user@debian-vm
```

## Listener configuration

Inside the container, the listeners bind `0.0.0.0` so Docker forwarding works.
Compose explicitly publishes to **host** `127.0.0.1`; Docker otherwise publishes
on all interfaces ([Docker documentation](https://docs.docker.com/get-started/docker-concepts/running-containers/publishing-ports/)).
The Mongo listener has no authentication. Containers sharing its bridge network
can also reach it, so do not attach untrusted containers to that network or use
host networking. The Mongo-to-SQL gateway is disabled for this non-loopback bind;
SQL clients can still use `mongo()`.

Native Debian and Homebrew installations keep the Mongo listener itself on
loopback. The container override is shipped only in the Docker image.
