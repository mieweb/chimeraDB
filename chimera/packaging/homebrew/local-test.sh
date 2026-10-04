#!/usr/bin/env bash
# Install the current checkout through a private local tap, without linking its
# command or starting a global service. brew test uses isolated state and ports.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
die() { printf 'homebrew local test: %s\n' "$*" >&2; exit 1; }
server= work="$REPO/chimera/.run/release/homebrew" reinstall=false
while (($#)); do
  case "$1" in
    --server) server=${2:?}; shift 2 ;;
    --work-dir) work=${2:?}; shift 2 ;;
    --reinstall) reinstall=true; shift ;;
    *) die "unknown argument: $1" ;;
  esac
done
case "$server" in
  11.8) formula=chimeradb ;;
  10.11) formula=chimeradb@10.11 ;;
  *) die '--server 10.11|11.8 is required' ;;
esac
[[ $(uname -s) == Darwin ]] || die 'requires macOS and Homebrew'
export HOMEBREW_NO_AUTO_UPDATE=1 HOMEBREW_NO_INSTALL_CLEANUP=1
mkdir -p "$work"
work=$(cd "$work" && pwd)
tap=chimera-local/release-validation
tap_repo=$(brew --repository)/Library/Taps/chimera-local/homebrew-release-validation
if [[ ! -d $tap_repo ]]; then
  brew tap-new "$tap"
fi
[[ -d $tap_repo/.git ]] || die "unexpected local tap layout: $tap_repo"

# Include current tracked and untracked source, but never .run/, build outputs,
# upstream repositories or credentials excluded by gitignore. Each content hash
# gets a distinct URL so Homebrew cannot reuse a previous working-tree archive.
python3 - "$REPO" "$work" <<'PY'
import hashlib
from pathlib import Path
import subprocess
import sys
import tarfile

repo, work = map(Path, sys.argv[1:])
version = (repo / "chimera/VERSION").read_text().strip()
files = subprocess.check_output([
    "git", "-C", str(repo), "ls-files", "--cached", "--others",
    "--exclude-standard", "-z", "--", "chimera/",
]).split(b"\0")
archive = work / "source.tar.gz"
with tarfile.open(archive, "w:gz") as tar:
    for entry in sorted(set(files)):
        if entry:
            relative = entry.decode()
            tar.add(repo / relative, arcname=f"chimeradb-{version}/{relative}", recursive=False)
checksum = hashlib.sha256(archive.read_bytes()).hexdigest()
destination = work / f"chimeradb-{version}-{checksum[:16]}.tar.gz"
archive.replace(destination)
(work / "source.url").write_text(destination.as_uri() + "\n")
(work / "source.sha256").write_text(checksum + "\n")
print(f"Source snapshot: {destination}")
PY
python3 "$HERE/render-formula.py" --url "$(cat "$work/source.url")" \
  --sha256 "$(cat "$work/source.sha256")" --output "$tap_repo/Formula"
# Reinstall evaluates conflicts_with and loads the sibling formula too. Trust
# only the two formulae just generated here, rather than the entire tap or all
# third-party code. Older Homebrew versions predate this trust command.
if brew command trust >/dev/null 2>&1; then
  brew trust --formula "$tap/chimeradb" "$tap/chimeradb@10.11"
fi
if $reinstall; then
  # Some Homebrew versions link non-keg-only formulae during reinstall even
  # when their previous keg was unlinked. Restore that initial state on every
  # exit, including a later build/test failure, without hiding the failure.
  linked_keg=$(brew info --json=v2 "$tap/$formula" | python3 -c \
    'import json, sys; print(json.load(sys.stdin)["formulae"][0]["linked_keg"] or "")')
  if [[ -z $linked_keg ]]; then
    restore_unlinked() {
      local status=$?
      trap - EXIT
      if ! brew unlink "$tap/$formula"; then
        printf 'homebrew local test: could not restore unlinked state for %s\n' "$tap/$formula" >&2
        if ((status == 0)); then status=1; fi
      fi
      exit "$status"
    }
    trap restore_unlinked EXIT
  fi
  brew reinstall --build-from-source "$tap/$formula"
else
  # Both series can remain installed while existing PATH links stay untouched.
  brew install --build-from-source --skip-link "$tap/$formula"
fi
# Homebrew explicitly supports testing unlinked formulae with --force; retain
# --skip-link above so this check never replaces an existing chimeradb command.
brew test --force "$tap/$formula"
printf 'Installed and tested %s from a local source snapshot. No global service was started.\n' "$tap/$formula"
