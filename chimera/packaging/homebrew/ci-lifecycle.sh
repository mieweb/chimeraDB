#!/usr/bin/env bash
# Dedicated clean-runner acceptance: launchd, reinstall and config/data survival.
# Unlike local-test.sh, this intentionally exercises the formula's real service.
set -euo pipefail
die() { printf 'homebrew lifecycle: %s\n' "$*" >&2; exit 1; }
[[ ${GITHUB_ACTIONS:-} == true && ${RUNNER_OS:-} == macOS ]] ||
  die 'run only on a clean GitHub Actions macOS runner after local-test.sh'
[[ $# == 2 && $1 == --server ]] || die 'usage: ci-lifecycle.sh --server 10.11|11.8'
case "$2" in
  11.8) formula=chimeradb ;;
  10.11) formula=chimeradb@10.11 ;;
  *) die 'unsupported MariaDB series' ;;
esac
server=$2
qualified="chimera-local/release-validation/$formula"
export HOMEBREW_NO_AUTO_UPDATE=1 HOMEBREW_NO_INSTALL_CLEANUP=1
brew_prefix=$(brew --prefix)
prefix=$(brew --prefix "$qualified")
maria=$(brew --prefix "mariadb@$server")
config="$brew_prefix/etc/$formula.cnf"
data="$brew_prefix/var/$formula"
[[ -x $prefix/bin/chimeradb && -f $config ]] || die 'install the local test formula first'
[[ ! -e $data ]] || die "refusing to use existing service data at $data"

# Reserve distinct free ports during discovery; only this formula's new config
# is changed, and the user-edit marker must survive the reinstall below.
read -r sql_port mongo_port < <(python3 - <<'PY'
import socket
with socket.socket() as sql, socket.socket() as mongo:
    sql.bind(("127.0.0.1", 0))
    mongo.bind(("127.0.0.1", 0))
    print(sql.getsockname()[1], mongo.getsockname()[1])
PY
)
python3 - "$config" "$sql_port" "$mongo_port" <<'PY'
from pathlib import Path
import sys
path = Path(sys.argv[1])
lines = path.read_text().splitlines()
for index, line in enumerate(lines):
    if line.startswith("port="):
        lines[index] = f"port={sys.argv[2]}"
    elif line.startswith("chimera-mongo-port="):
        lines[index] = f"chimera-mongo-port={sys.argv[3]}"
lines.append("# ChimeraDB lifecycle test: preserve this local customization")
path.write_text("\n".join(lines) + "\n")
PY
before=$(shasum -a 256 "$config" | awk '{print $1}')

# Same upstream version, different Homebrew keg revision: refuse before data
# initialization. Restore metadata even when the regression assertion fails.
python3 - "$prefix" "$maria" "$config" "$data" <<'PY'
import os
from pathlib import Path
import signal
import subprocess
import sys

prefix, maria, config, data = map(Path, sys.argv[1:])
marker = prefix / "share/chimeradb/mariadb-prefix"
original = marker.read_text()
env = dict(os.environ, CHIMERA_PREFIX=str(prefix), CHIMERA_MARIADB_PREFIX=str(maria),
           CHIMERA_DEFAULTS_FILE=str(config), CHIMERA_DATA_DIR=str(data))
try:
    marker.write_text(original.strip() + "-stale-revision\n")
    process = subprocess.Popen([str(prefix / "libexec/chimeradb-service")],
                               env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               start_new_session=True)
    try:
        output, _ = process.communicate(timeout=10)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGTERM)
        try:
            process.communicate(timeout=15)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.communicate()
        raise RuntimeError("stale MariaDB keg was allowed to start")
    assert process.returncode != 0, "stale MariaDB keg was accepted"
    assert b"plugin was built for MariaDB keg" in output, output.decode()
    assert not data.exists(), "stale MariaDB keg initialized service data"
    print("PASS: stale MariaDB keg rejected before initialization")
finally:
    marker.write_text(original)
PY
running=false
cleanup() {
  if $running; then
    brew services stop "$qualified" || true
  fi
}
trap cleanup EXIT
sql() { "$maria/bin/mariadb" --defaults-file="$config" --protocol=socket --user="$(id -un)" -N -B "$@"; }
start() {
  # run exercises launchd without registering the service to start at login.
  running=true
  brew services run "$qualified"
  for ((i=0; i<120; i++)); do
    if "$prefix/bin/chimeradb" status --protocol=socket --user="$(id -un)" >/dev/null 2>&1; then
      return
    fi
    sleep 1
  done
  [[ ! -f $data/mariadbd.err ]] || cat "$data/mariadbd.err" >&2
  [[ ! -f $brew_prefix/var/log/$formula.log ]] || cat "$brew_prefix/var/log/$formula.log" >&2
  die 'launchd service did not become ready'
}
stop() {
  brew services stop "$qualified"
  running=false
  for ((i=0; i<60; i++)); do
    if ! sql -e 'SELECT 1' >/dev/null 2>&1; then
      return
    fi
    sleep 1
  done
  die 'server did not stop cleanly'
}
start
uri="mongodb://127.0.0.1:$mongo_port/?directConnection=true&serverSelectionTimeoutMS=5000"
"$prefix/libexec/chimeradb-wire-smoke" "$uri"
[[ $(sql -e 'SELECT COUNT(*) FROM package_smoke.wire_docs') == 1 ]] || die 'launchd driver write was not visible in SQL'
stop

brew reinstall --build-from-source "$qualified"
[[ $(shasum -a 256 "$config" | awk '{print $1}') == "$before" ]] || die 'reinstall changed the customized config'
brew test --force "$qualified"
start
"$prefix/libexec/chimeradb-wire-smoke" "$uri" --ping-only
[[ $(sql -e 'SELECT COUNT(*) FROM package_smoke.wire_docs') == 1 ]] || die 'reinstall lost service data'
stop
printf 'PASS: %s launchd service, reinstall, config preservation and data persistence\n' "$qualified"
