# Signed Debian APT repository

This directory supplies the publishing and installation tooling for a public APT
repository. **It has not been published yet.** The commands below become an
installation route only after the first successful deployment and publication of
the real signing-key fingerprint. No Homebrew tap or Docker image is published
by this workflow.

The initial repository serves native **amd64** packages:

| Debian release | Suite | MariaDB dependency |
|---|---|---|
| Debian 12 | `bookworm` | Debian's MariaDB 10.11 |
| Debian 13 | `trixie` | Debian's MariaDB 11.8 |

Each suite has its own `main` component and package pool. The existing Debian
build recipes still support other documented combinations; Debian 12 with
MariaDB.org 11.8 and native arm64 are outside this first public APT repository.
The helper rejects unsupported operating systems and architectures before
changing APT configuration.

## Install after publication

Install the bootstrap dependencies from Debian, download the repository helper,
and run it with the **published full signing-key fingerprint**:

```sh
sudo apt-get update
sudo apt-get install -y ca-certificates curl gnupg
curl --fail --location --output /tmp/chimeradb-setup.sh \
  https://mieweb.github.io/chimeraDB/apt/setup-repository.sh
# Replace PUBLISHED_FINGERPRINT with the actual fingerprint from this repository.
sudo bash /tmp/chimeradb-setup.sh --key-fingerprint PUBLISHED_FINGERPRINT
sudo apt-get install chimeradb
sudo systemctl restart mariadb
sudo chimeradb setup
sudo chimeradb status
```

`PUBLISHED_FINGERPRINT` is deliberately invalid until the real public key is
available. Do not copy these commands as an available installation route yet.
If GitHub Pages uses an organization custom domain, substitute the deployed URL
and pass `--repository-url https://YOUR-DOMAIN/chimeraDB/apt` to the helper.

The helper verifies the downloaded public key, installs it under
`/etc/apt/keyrings/`, creates a distribution-specific deb822 source with
`Signed-By`, and updates APT's index. It does not install packages or restart an
existing database. APT verifies the signed repository metadata and package
checksums. Do not use `apt-key` or `trusted=yes`.

Package installation preserves existing MariaDB accounts and data. Restart and
setup are explicit because an existing server needs a maintenance window. The
Mongo listener remains on `127.0.0.1:27017`. Later compatible releases arrive
through the normal APT upgrade process.

## One-time publisher setup

1. Choose a dedicated OpenPGP signing key owned by ChimeraDB's maintainers. Keep
   a recoverable offline copy. The private key never belongs in this repository,
   a release attachment, an issue, or a chat message.
2. In repository or `github-pages` environment settings, add the ASCII-armored
   private key as the GitHub Actions secret **`APT_SIGNING_KEY`**. Add its
   passphrase as **`APT_SIGNING_PASSPHRASE`** if it has one. Add its full primary
   fingerprint as the Actions variable **`APT_SIGNING_FINGERPRINT`**.
3. Enable GitHub Pages for this repository, selecting **GitHub Actions** as the
   build source. If `github-pages` has branch restrictions, allow deployments
   from `main`. Environment reviewers can protect production publication.
4. Merge the publishing tooling, then create a release tag such as `v0.1.0` at a
   reviewed commit reachable from `main`. The tag must match `chimera/VERSION`.
5. Run **publish APT repository** from `main`, supplying the tag and a positive
   Debian packaging revision, for example `3`. A new rebuild must use a larger
   revision than the currently published version, even if only MariaDB changed.
6. Check the deployment URL, then perform a fresh installation on each supported
   Debian release through HTTPS. Publish the real public-key fingerprint and the
   verified install commands in the main README only after those checks pass.

The workflow resolves the tag once to a full commit, builds both package sets
from that commit, and runs their existing package lifecycle tests. The signing
job validates package manifests and identities again, signs both suite indexes,
and installs from the resulting repository in fresh Debian containers. It then
retains the complete repository on the `apt-repository` branch and deploys the
same files to GitHub Pages. Publications are serialized. PR runs exercise the
repository and bootstrap regressions without access to a production key.

The retained branch is distribution output, not a source branch. Existing
signed metadata, package checksums and identities are revalidated before reuse.
Previous package versions remain available. A publication refuses a downgrade
or different bytes under an existing package identity/version. Debug-symbol
packages are validated as build inputs but excluded from the public runtime
repository. Signed provenance records identify the source commit and exact
MariaDB package used for every package set.

## MariaDB updates and signing-key rotation

The plugin uses MariaDB internals, so its dependency remains pinned to the exact
server package used at build time. Re-run publication with a higher packaging
revision after a MariaDB update. APT must be able to resolve the exact server
dependency from Debian's configured repositories; the workflow's fresh-container
installation checks this before deployment. Holding back a MariaDB update until
a matching plugin is available is preferable to loading an incompatible binary.
This workflow does not schedule unattended publications or change that ABI rule.

The same public key is required to validate an existing repository snapshot.
Key rotation therefore requires a separately reviewed migration that distributes
the new public key to installed clients and verifies the old snapshot before
re-signing it. Replacing a secret alone is not a key-rotation procedure.

## Local verification

On Debian with `apt-utils`, `dpkg-dev`, `gnupg` and Python 3 installed:

```sh
python3 chimera/packaging/apt/test-repository.py
python3 chimera/packaging/apt/test-setup.py
```

The tests generate disposable signing keys and real Debian package fixtures;
they do not use production signing credentials. Build the actual packages using
the [Debian recipes](../deb/README.md), then generate a repository with an
isolated test key:

```sh
python3 chimera/packaging/apt/generate-repository.py \
  --package-set bookworm=/absolute/path/to/bookworm/10.11/amd64 \
  --package-set trixie=/absolute/path/to/trixie/11.8/amd64 \
  --source-ref "$(git rev-parse HEAD)" --signing-key FULL_TEST_KEY_FINGERPRINT \
  --output /tmp/chimeradb-apt
./chimera/packaging/apt/test-install.sh --repository /tmp/chimeradb-apt --suite bookworm
./chimera/packaging/apt/test-install.sh --repository /tmp/chimeradb-apt --suite trixie
```

The install tests mount the repository read-only into disposable amd64 Docker
containers and use APT's signature validation and dependency resolver. They
initialize private test databases, verify readiness, and exercise an SQL/Mongo
write. They never install software or modify databases on the host.
