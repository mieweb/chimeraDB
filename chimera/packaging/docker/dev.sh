#!/usr/bin/env bash
# Run any chimera script on Debian, against this checkout (M9.0.2).
#
#   dev.sh [--arch amd64|arm64] [--suite bookworm] [--] [command...]
#
# With no command you get a shell. The checkout is bind-mounted, so the scripts
# under test are the ones in your working tree; build products land in a named
# volume instead, which is why the host's macOS build survives a container run.

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../../.." && pwd)

die() { printf 'error: %s\n' "$*" >&2; exit 1; }

case "$(uname -m)" in
  arm64|aarch64) arch=arm64 ;;
  x86_64|amd64) arch=amd64 ;;
  *) arch="" ;;
esac
suite=bookworm

while (($#)); do
  case "$1" in
    --arch) arch="${2:-}"; shift 2 ;;
    --arch=*) arch="${1#*=}"; shift ;;
    --suite) suite="${2:-}"; shift 2 ;;
    --suite=*) suite="${1#*=}"; shift ;;
    --) shift; break ;;
    *) break ;;
  esac
done

[[ $arch == amd64 || $arch == arm64 ]] || die "unknown --arch '$arch' (expected amd64 or arm64)"

image="chimera-dev-debian:$suite-$arch"
volume="chimera-dev-$suite-$arch"

printf '==> building %s\n' "$image"
docker build --platform "linux/$arch" --build-arg "SUITE=$suite" \
  -f "$SCRIPT_DIR/dev-debian.Dockerfile" -t "$image" "$SCRIPT_DIR"

tty_args=()
[[ -t 0 ]] && tty_args=(-it)

(($#)) || set -- bash
printf '==> %s: %s\n' "$image" "$*"
exec docker run --rm "${tty_args[@]+"${tty_args[@]}"}" --platform "linux/$arch" \
  -v "$REPO_ROOT:/work" -v "$volume:/out" -w /work "$image" "$@"
