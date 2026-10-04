#!/usr/bin/env bash
# Native lifecycle acceptance; THIS INSTALLS PACKAGES AND USES THE REAL SERVICE.
# Run as root only on an explicitly disposable, fresh Debian systemd host.
# Success leaves the current ChimeraDB packages installed, MariaDB running, and
# the acceptance data available. Failures leave state for inspection, not cleanup.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
die() { printf 'systemd acceptance: %s\n' "$*" >&2; exit 1; }
usage() {
  cat <<'EOF'
usage: systemd-test.sh --ack-disposable-host --series 10.11|11.8 --packages DIR
                       [--previous-packages DIR]

Requires root and a fresh Debian 12/13 host with systemd. Refuses existing
MariaDB/MySQL/Chimera packages, configuration and data. Optional previous
packages test a real ChimeraDB package-version upgrade against the SAME
MariaDB server package. Installs Python driver packages for acceptance.

On success: current ChimeraDB remains installed, MariaDB is running, SQL root
uses local socket authentication, and Mongo binds 127.0.0.1:27017. The database
chimera_systemd_acceptance and systemd/APT logs remain. No database data is deleted.
EOF
}
ack=false
series= packages= previous=
while (($#)); do
  case $1 in
    --ack-disposable-host) ack=true; shift ;;
    --series|--server) series=${2:?missing series}; shift 2 ;;
    --packages) packages=${2:?missing package directory}; shift 2 ;;
    --previous-packages) previous=${2:?missing previous package directory}; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown argument: $1" ;;
  esac
done
$ack || die 'requires --ack-disposable-host; this harness changes the native database service'
[[ $EUID == 0 ]] || die 'run as root on the disposable host'
[[ $series == 10.11 || $series == 11.8 ]] || die 'select --series 10.11 or 11.8'
[[ -n $packages ]] || die '--packages is required'
[[ -d /run/systemd/system ]] || die 'the host must boot systemd as its service manager'
system_state=$(systemctl is-system-running 2>/dev/null || true)
[[ $system_state == running || $system_state == degraded ]] ||
  die "systemd is not ready (state: $system_state)"
. /etc/os-release
[[ $ID == debian && ( $VERSION_CODENAME == bookworm || $VERSION_CODENAME == trixie ) ]] ||
  die 'only Debian 12/bookworm or 13/trixie is supported'
arch=$(dpkg --print-architecture)
[[ $arch == amd64 || $arch == arm64 ]] || die "unsupported host architecture: $arch"
distribution="Debian $VERSION_ID ($VERSION_CODENAME)"
export LC_ALL=C DEBIAN_FRONTEND=noninteractive

# These checks all precede APT, repository configuration, directory creation or
# any service mutation. A previously attempted run is intentionally refused.
existing=$(dpkg-query -W -f='${binary:Package}\t${Status}\n' | awk '
  $1 ~ /^(mariadb|libmariadb|mysql|default-mysql|galera|chimeradb)/ &&
  $0 !~ / not-installed$/ { print }')
[[ -z $existing ]] || die "database packages already exist; use a fresh host:\n$existing"
for path in /var/lib/mysql /etc/mysql /run/mysqld; do
  [[ ! -e $path && ! -L $path ]] || die "existing database path: $path"
done
! command -v mariadbd >/dev/null 2>&1 || die 'an existing MariaDB binary is on PATH'
! command -v mysqld >/dev/null 2>&1 || die 'an existing MySQL binary is on PATH'
command -v ss >/dev/null 2>&1 || die 'iproute2 (ss) is required for the preflight port check'
[[ -z $(ss -H -ltn '( sport = :3306 or sport = :27017 )') ]] ||
  die 'TCP 3306 or 27017 is already in use; stop the conflicting test deployment first'

packages=$(cd "$packages" && pwd)
[[ -z $previous ]] || previous=$(cd "$previous" && pwd)
validate_packages() {
  local directory=$1 file package version package_arch common=0 meta=0 plugin=0
  [[ -f $directory/SHA256SUMS && -f $directory/build-info.txt ]] ||
    die "missing checksums/build metadata: $directory"
  [[ $(sed -n 's/^MariaDB series: //p' "$directory/build-info.txt") == "$series" ]] ||
    die "wrong MariaDB series: $directory"
  [[ $(sed -n 's/^Architecture: //p' "$directory/build-info.txt") == "$arch" ]] ||
    die "packages do not match host architecture $arch: $directory"
  [[ $(sed -n 's/^Distribution: //p' "$directory/build-info.txt") == "$distribution" ]] ||
    die "packages do not match host distribution $distribution: $directory"
  # The exact generated manifest must cover every .deb APT will receive; a
  # valid checksum for only a subset must not permit unverified extra packages.
  (cd "$directory" && sha256sum --check --strict SHA256SUMS >&2 &&
    cmp <(sort SHA256SUMS) <(sha256sum ./*.deb | sort)) ||
    die "checksum manifest mismatch: $directory"
  local plugins=("$directory/chimeradb-plugin-${series}_"*.deb)
  [[ ${#plugins[@]} == 1 && -f ${plugins[0]} ]] || die "expected one plugin: $directory"
  version=$(dpkg-deb -f "${plugins[0]}" Version)
  [[ $(sed -n 's/^ChimeraDB: //p' "$directory/build-info.txt") == "$version" ]] ||
    die "ChimeraDB package version does not match build metadata: $directory"
  local server_version
  server_version=$(sed -n 's/^MariaDB package: //p' "$directory/build-info.txt")
  [[ -n $server_version && ${server_version#*:} == "$series".* ]] ||
    die "invalid MariaDB package metadata: $directory"
  [[ $(dpkg-deb -f "${plugins[0]}" Depends) == *"mariadb-server (= $server_version)"* ]] ||
    die "plugin dependency does not match recorded MariaDB package: $directory"
  for file in "$directory"/*.deb; do
    package=$(dpkg-deb -f "$file" Package)
    package_arch=$(dpkg-deb -f "$file" Architecture)
    case $package in
      chimeradb) meta=$((meta + 1)); [[ $package_arch == all ]] || die "wrong architecture: $file" ;;
      chimeradb-common) common=$((common + 1)); [[ $package_arch == all ]] || die "wrong architecture: $file" ;;
      "chimeradb-plugin-$series") plugin=$((plugin + 1)); [[ $package_arch == "$arch" ]] || die "wrong architecture: $file" ;;
      "chimeradb-plugin-$series-dbgsym") [[ $package_arch == "$arch" ]] || die "wrong architecture: $file" ;;
      *) die "unexpected package $package in $directory" ;;
    esac
    [[ $(dpkg-deb -f "$file" Version) == "$version" ]] || die "mixed versions: $directory"
  done
  [[ $meta == 1 && $common == 1 && $plugin == 1 ]] || die "incomplete package set: $directory"
  printf '%s\n' "$version"
}
current_version=$(validate_packages "$packages")
server_version=$(sed -n 's/^MariaDB package: //p' "$packages/build-info.txt")
initial_packages=$packages
initial_version=$current_version
if [[ -n $previous ]]; then
  initial_version=$(validate_packages "$previous")
  dpkg --compare-versions "$current_version" gt "$initial_version" ||
    die 'current packages must be newer than --previous-packages'
  [[ $(sed -n 's/^MariaDB package: //p' "$previous/build-info.txt") == "$server_version" ]] ||
    die 'package upgrade acceptance requires the same MariaDB server version in both sets'
  initial_packages=$previous
fi

phase='repository and package installation'
diagnose() {
  local result=$?
  if [[ $result != 0 ]]; then
    printf '\nFAILED during %s; preserving service/data for inspection.\n' "$phase" >&2
    systemctl status mariadb --no-pager >&2 || true
    journalctl -u mariadb -n 60 --no-pager >&2 || true
  fi
}
trap diagnose EXIT
"$HERE/configure-repository.sh" "$series"
apt_options=(-y -o Dpkg::Options::=--force-confdef -o Dpkg::Options::=--force-confold)
install_packages() {
  local directory=$1
  shift
  apt-get "${apt_options[@]}" install --no-install-recommends "$@" "$directory"/*.deb
}
install_packages "$initial_packages"
apt-get "${apt_options[@]}" install --no-install-recommends python3-pymongo python3-pymysql
[[ $(dpkg-query -W -f='${Version}' mariadb-server) == "$server_version" ]] || die 'wrong MariaDB server installed'
[[ $(dpkg-query -W -f='${Version}' "chimeradb-plugin-$series") == "$initial_version" ]] || die 'wrong initial plugin installed'
# These packages must remain when only ChimeraDB is removed; never autoremove.
apt-mark manual mariadb-server mariadb-client >/dev/null

socket=/run/mysqld/mysqld.sock
config=/etc/mysql/mariadb.conf.d/60-chimera.cnf
sql() { mariadb --no-defaults --protocol=socket --socket="$socket" --user=root -N -B "$@"; }
restart_service() {
  systemctl restart mariadb
  for ((attempt=0; attempt<60; attempt++)); do
    if systemctl is-active --quiet mariadb && sql -e 'SELECT 1' >/dev/null 2>&1; then return; fi
    sleep 1
  done
  die 'MariaDB did not become ready through systemd'
}
check_sql_data() {
  [[ $(sql -e "SELECT COUNT(*) FROM chimera_systemd_acceptance.items WHERE _id=CONCAT(0x02,'persisted')") == 1 ]] ||
    die 'persistent document missing from SQL'
}
verify_native() {
  chimeradb status --protocol=socket --socket="$socket" --user=root
  [[ $(sql -e 'SELECT @@chimera_mongo_bind') == 127.0.0.1 ]] || die 'Mongo listener is not loopback'
  python3 "$HERE/systemd-smoke.py" --socket "$socket" "$@"
}
phase='systemd startup, setup and driver acceptance'
restart_service
chimeradb setup --protocol=socket --socket="$socket" --user=root
chimeradb setup --protocol=socket --socket="$socket" --user=root
verify_native --seed
phase='service restart and persistent data'
restart_service
verify_native
printf '\n# systemd acceptance: preserve this administrator customization\n' >>"$config"
config_hash=$(sha256sum "$config" | awk '{print $1}')

if [[ -n $previous ]]; then
  phase='package version upgrade'
  systemctl stop mariadb
  install_packages "$packages"
  [[ $(dpkg-query -W -f='${Version}' "chimeradb-plugin-$series") == "$current_version" ]] || die 'package upgrade installed wrong version'
  [[ $(sha256sum "$config" | awk '{print $1}') == "$config_hash" ]] || die 'upgrade changed customized configuration'
  restart_service
  verify_native
  printf 'PASS: actual package version upgrade %s -> %s\n' "$initial_version" "$current_version"
fi

phase='same-version reinstall and configuration preservation'
systemctl stop mariadb
install_packages "$packages" --reinstall
[[ $(sha256sum "$config" | awk '{print $1}') == "$config_hash" ]] || die 'reinstall changed customized configuration'
restart_service
verify_native

# Inspect APT's proposed removals before executing; no non-Chimera package may
# be removed by this harness, even if the local APT configuration differs.
remove_chimera() {
  local operation=$1 proposed package
  shift
  proposed=$(apt-get -s -o APT::Get::AutomaticRemove=false "$operation" "$@")
  while read -r package; do
    [[ -z $package || $package == chimeradb || $package == chimeradb-* ]] ||
      die "APT proposed removing non-Chimera package: $package"
  done < <(printf '%s\n' "$proposed" | awk '$1 == "Remv" || $1 == "Purg" {print $2}')
  apt-get -y -o APT::Get::AutomaticRemove=false "$operation" "$@"
}
phase='Chimera removal with retained configuration and data'
systemctl stop mariadb
remove_chimera remove chimeradb "chimeradb-plugin-$series"
[[ -f $config ]] || die 'ordinary removal discarded the conffile'
restart_service
check_sql_data
[[ $(sql -e "SELECT COUNT(*) FROM information_schema.plugins WHERE plugin_name='chimera_mongo'") == 0 ]] || die 'plugin remained loaded after removal/restart'

phase='Chimera purge with MariaDB and data preserved'
systemctl stop mariadb
remove_chimera purge "chimeradb-plugin-$series" chimeradb-common
[[ ! -e $config ]] || die 'purge retained the package conffile'
restart_service
check_sql_data
[[ $(dpkg-query -W -f='${Version}' mariadb-server) == "$server_version" ]] || die 'MariaDB package changed during removal/purge'

phase='restore current ChimeraDB and leave service usable'
systemctl stop mariadb
install_packages "$packages"
restart_service
chimeradb setup --protocol=socket --socket="$socket" --user=root
verify_native
systemctl enable mariadb >/dev/null
printf 'PASS: native %s/%s systemd install, setup, drivers, restart, reinstall, remove/purge and data preservation\n' "$distribution" "$arch"
printf 'LEFT RUNNING: MariaDB %s, ChimeraDB %s; Mongo 127.0.0.1:27017; SQL root via %s\n' "$server_version" "$current_version" "$socket"
[[ -n $previous ]] || printf 'NOT TESTED: package-version upgrade (no --previous-packages provided)\n'
