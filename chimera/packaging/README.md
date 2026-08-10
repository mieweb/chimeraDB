# Packaging

Everything that gets ChimeraDB onto a machine that is not the one it was written on.
Owned by [release-plan.md](../../release-plan.md) (M9).

| Directory | What anchors it |
|---|---|
| `docker/` | The **Linux dev path**: a Debian image that runs `chimera/scripts/*` unmodified. Not a shipping artifact — it is how we find out what is macOS-only. |

Nothing here is required to develop on macOS. `chimera/scripts/` remains the only entry
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
