#!/usr/bin/env bash
# Tests a built image using only disposable containers and a fresh named volume.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
[[ $# == 2 && $1 == --image ]] || { echo 'usage: test.sh --image IMAGE' >&2; exit 1; }
image=$2
docker info >/dev/null 2>&1 || { echo 'Docker is not running' >&2; exit 1; }
name="chimera-release-$(date +%s)-$$"
volume="$name-data"
network="$name-net"
runner=chimeradb-smoke:bookworm
export MARIADB_ROOT_PASSWORD="release-smoke-$name"$'\'\\\nedge'
cleanup() {
  docker rm -f "$name" >/dev/null 2>&1 || true
  docker network rm "$network" >/dev/null 2>&1 || true
  docker volume rm "$volume" >/dev/null 2>&1 || true
}
trap cleanup EXIT
docker build -f "$HERE/smoke.Dockerfile" -t "$runner" "$HERE"
docker network create "$network" >/dev/null
docker volume create "$volume" >/dev/null
start() {
  docker run -d --name "$name" --network "$network" --network-alias db \
    -e MARIADB_ROOT_PASSWORD -v "$volume:/var/lib/mysql" \
    -p 127.0.0.1::3306 -p 127.0.0.1::27017 "$image" >/dev/null
  for ((attempt=0; attempt<180; attempt++)); do
    state=$(docker inspect --format '{{.State.Status}}/{{if .State.Health}}{{.State.Health.Status}}{{end}}' "$name")
    case $state in
      running/healthy) return 0 ;;
      exited/*|dead/*) break ;;
    esac
    sleep 1
  done
  docker logs "$name" >&2
  echo 'Image did not become healthy' >&2
  return 1
}
start
docker port "$name" 3306/tcp | grep -q '^127\.0\.0\.1:'
docker port "$name" 27017/tcp | grep -q '^127\.0\.0\.1:'
docker run --rm --network "$network" -e MARIADB_ROOT_PASSWORD "$runner"
docker stop --time 60 "$name" >/dev/null
[[ $(docker inspect --format '{{.State.ExitCode}}' "$name") == 0 ]]
docker rm "$name" >/dev/null
# A fresh container sees the same volume and must not initialize it again.
start
docker run --rm --network "$network" -e MARIADB_ROOT_PASSWORD "$runner" --verify-persistence
docker stop --time 60 "$name" >/dev/null
[[ $(docker inspect --format '{{.State.ExitCode}}' "$name") == 0 ]]
printf 'PASS: %s, host loopback publishing, graceful stop and volume persistence\n' "$image"
