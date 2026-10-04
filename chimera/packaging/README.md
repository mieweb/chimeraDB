# Packaging

Everything that gets ChimeraDB onto a machine that is not the one it was written on.
Owned by [release-plan.md](../../release-plan.md) (M9).

| Directory | What anchors it |
|---|---|
| [docker/](docker/README.md) | Runtime image and Compose for Mac arm64 and Intel Linux, plus the Debian development environment. |
| [homebrew/](homebrew/README.md) | Source-only formula generation, a dedicated macOS service and staged install checks. |
| [deb/](deb/README.md) | Debian 12 packages, exact server dependencies and install/remove validation. |

The Mac images and ARM Debian package lifecycle pass on both server series under
OrbStack. Homebrew installation and Intel VM acceptance are tracked in the
[release plan](../../release-plan.md). Nothing here is a published release yet.

Nothing here is required to develop against the existing macOS source builds. `chimera/scripts/` remains the entry
point for the normal loop; the image below just runs those same scripts on Debian.

```sh
docker/dev.sh -- ./chimera/packaging/docker/build-server.sh --server 10.11   # once, slow
docker/dev.sh -- bash -lc './chimera/scripts/run-server.sh --server 10.11 && \
                           ./chimera/scripts/demo-m1.sh --server 10.11'
```

Each `dev.sh` call is one container: a server started by one call is gone by the next, so a
server and the scripts that talk to it belong in the same invocation. Build products go to a
named volume (`CHIMERA_OUT=/out`), never into the bind-mounted checkout, so a container build
and the host's macOS build coexist.

One thing they cannot share: [link-plugin.sh](../scripts/link-plugin.sh) writes an *absolute*
symlink into the server trees, which are bind-mounted. Whichever platform ran it last owns
the link, and the other sees it dangling. Re-running the script fixes it, and every build
script does.
