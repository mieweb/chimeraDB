#!/usr/bin/env bash
# Deterministic entrypoint races using only disposable volumes and containers.
# No real initializer or server runs: install-db/gosu are failing test fixtures.
set -euo pipefail
[[ $# == 2 && $1 == --image ]] || { echo 'usage: test-initialization.sh --image IMAGE' >&2; exit 1; }
image=$2
docker info >/dev/null 2>&1 || { echo 'Docker is not running' >&2; exit 1; }
architecture=$(docker image inspect --format '{{.Architecture}}' "$image")
[[ $architecture == arm64 || $architecture == amd64 ]] || { echo 'Unsupported image architecture' >&2; exit 1; }
prefix="chimera-init-$(date +%s)-$$"
sync_volume="$prefix-sync"
race_volume="$prefix-race"
late_volume="$prefix-late"
existing_volume="$prefix-existing"
fixtures=$(mktemp -d "${TMPDIR:-/tmp}/chimera-init-fixtures.XXXXXX")
containers=("$prefix-one" "$prefix-two" "$prefix-retry" "$prefix-late" "$prefix-existing")
volumes=("$sync_volume" "$race_volume" "$late_volume" "$existing_volume")
cleanup() {
  local result=$?
  if ((result != 0)); then
    for container in "${containers[@]}"; do
      if docker inspect "$container" >/dev/null 2>&1; then docker logs "$container" >&2 || true; fi
    done
  fi
  docker rm -f "${containers[@]}" >/dev/null 2>&1 || true
  docker volume rm "${volumes[@]}" >/dev/null 2>&1 || true
  rm -rf "$fixtures"
}
trap cleanup EXIT
die() { printf 'initialization regression: %s\n' "$*" >&2; exit 1; }

# This is after the entrypoint's first mysql/ and empty-directory checks, but
# before it claims the initialization marker. Readiness/release files use a
# separate volume so they cannot affect the data-directory checks under test.
cat >"$fixtures/chown" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
if [[ $# == 2 && $1 == mysql:mysql && $2 == /var/lib/mysql ]]; then
  touch "/test-sync/$CHIMERA_TEST_ID.ready"
  for ((attempt=0; attempt<300; attempt++)); do
    if [[ -f /test-sync/$CHIMERA_TEST_ID.go ]]; then exec /usr/bin/chown "$@"; fi
    sleep 0.1
  done
  echo 'fixture barrier was not released' >&2
  exit 95
fi
exec /usr/bin/chown "$@"
EOF
cat >"$fixtures/mariadb-install-db" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$CHIMERA_TEST_ID" >"/test-sync/$CHIMERA_TEST_ID.initialized"
mkdir -p /var/lib/mysql/mysql
exit 73
EOF
cat >"$fixtures/gosu" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
touch "/test-sync/$CHIMERA_TEST_ID.server-started"
exit 74
EOF
# Reproduce the separate existing-directory race after the initial marker
# guard, immediately before the mysql/ branch. The sentinel makes a stale hook
# fail the test instead of silently ceasing to exercise this schedule.
cat >"$fixtures/existing-race.env" <<'EOF'
trap 'if [[ $BASH_COMMAND == "[[ ! -d \$datadir/mysql ]]" ]]; then
  mkdir -p /var/lib/mysql/mysql
  touch /var/lib/mysql/.chimera-initializing /test-sync/existing.injected
  trap - DEBUG
fi' DEBUG
EOF
chmod 755 "$fixtures/chown" "$fixtures/mariadb-install-db" "$fixtures/gosu"
for volume in "${volumes[@]}"; do docker volume create "$volume" >/dev/null; done
start_fixture() {
  local id=$1 data_volume=$2
  shift 2
  docker run -d --platform "linux/$architecture" --name "$prefix-$id" \
    --network none --no-healthcheck \
    --env MARIADB_ROOT_PASSWORD=disposable-initialization-fixture \
    --env "CHIMERA_TEST_ID=$id" --env PATH=/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
    --mount "type=volume,src=$data_volume,dst=/var/lib/mysql" \
    --mount "type=volume,src=$sync_volume,dst=/test-sync" \
    --mount "type=bind,src=$fixtures,dst=/test-fixtures,readonly" \
    --mount "type=bind,src=$fixtures/chown,dst=/usr/local/bin/chown,readonly" \
    --mount "type=bind,src=$fixtures/mariadb-install-db,dst=/usr/local/bin/mariadb-install-db,readonly" \
    --mount "type=bind,src=$fixtures/gosu,dst=/usr/local/bin/gosu,readonly" \
    "$@" "$image" >/dev/null
}
control() {
  local data_volume=$1
  shift
  docker run --rm --platform "linux/$architecture" --network none --no-healthcheck \
    --entrypoint bash --mount "type=volume,src=$data_volume,dst=/var/lib/mysql" \
    --mount "type=volume,src=$sync_volume,dst=/test-sync" "$image" -e -u -o pipefail "$@"
}
wait_for_barrier() {
  local id=$1 other=${2:-$1}
  docker exec "$prefix-$id" bash -c '
    for ((attempt=0; attempt<200; attempt++)); do
      [[ -f /test-sync/$1.ready && -f /test-sync/$2.ready ]] && exit 0
      sleep 0.05
    done
    exit 1
  ' bash "$id" "$other" || die "contenders did not reach the pre-claim barrier: $id/$other"
}

start_fixture one "$race_volume"
start_fixture two "$race_volume"
wait_for_barrier one two
control "$race_volume" -c 'touch /test-sync/one.go /test-sync/two.go'
statuses=$(docker wait "$prefix-one" "$prefix-two" | LC_ALL=C sort)
[[ $statuses == $'1\n73' ]] || die "expected one rejected contender and one fixture initializer, got: $statuses"
control "$race_volume" -c '
  set -e
  count=$(find /test-sync -maxdepth 1 -name "*.initialized" | wc -l)
  [[ $count == 1 && -d /var/lib/mysql/mysql && -f /var/lib/mysql/.chimera-initializing ]]
  [[ ! -e /test-sync/one.server-started && ! -e /test-sync/two.server-started ]]
' || die 'concurrent claim did not preserve exactly one interrupted initializer'
start_fixture retry "$race_volume"
[[ $(docker wait "$prefix-retry") == 1 ]] || die 'partial volume was accepted on a subsequent launch'
control "$race_volume" -c '
  [[ -f /var/lib/mysql/.chimera-initializing && ! -e /test-sync/retry.initialized && ! -e /test-sync/retry.server-started ]]
' || die 'retry changed partial initialization or reached a server'
echo 'PASS: simultaneous empty-volume claims run one initializer; interrupted volume remains protected'

# B has already observed an empty volume when A finishes and removes its marker.
# Seed A's completed state while B is paused, then let B claim and recheck it.
start_fixture late "$late_volume"
wait_for_barrier late
control "$late_volume" -c '
  mkdir /var/lib/mysql/mysql
  printf "completed-by-another-initializer\n" >/var/lib/mysql/mysql/sentinel
  touch /test-sync/late.go
'
[[ $(docker wait "$prefix-late") == 1 ]] || die 'delayed contender accepted a volume completed by another initializer'
control "$late_volume" -c '
  [[ $(cat /var/lib/mysql/mysql/sentinel) == completed-by-another-initializer ]]
  [[ ! -e /var/lib/mysql/.chimera-initializing && ! -e /test-sync/late.initialized && ! -e /test-sync/late.server-started ]]
' || die 'delayed contender changed completed data or retained its own marker'
echo 'PASS: delayed contender rechecks data after claiming and preserves completed initialization'

start_fixture existing "$existing_volume" --env BASH_ENV=/test-fixtures/existing-race.env
[[ $(docker wait "$prefix-existing") == 1 ]] || die 'existing-directory race reached an initializer or server'
control "$existing_volume" -c '
  [[ -f /test-sync/existing.injected && -d /var/lib/mysql/mysql && -f /var/lib/mysql/.chimera-initializing ]]
  [[ ! -e /test-sync/existing.initialized && ! -e /test-sync/existing.server-started ]]
' || die 'existing-directory race did not preserve the competing initialization marker'
echo 'PASS: mysql/ appearing after the first marker guard does not bypass the final guard'
