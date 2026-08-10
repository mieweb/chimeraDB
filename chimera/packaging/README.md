# Packaging

Everything that gets ChimeraDB onto a machine that is not the one it was written on.
Owned by [release-plan.md](../../release-plan.md) (M9).

| Directory | What anchors it |
|---|---|
| `docker/` | The **Linux dev path**: a Debian image that runs `chimera/scripts/*` unmodified. Not a shipping artifact — it is how we find out what is macOS-only. |

Nothing here is required to develop on macOS. `chimera/scripts/` remains the only entry
point for the normal loop; the image below just runs those same scripts on Debian.
