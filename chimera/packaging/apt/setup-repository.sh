#!/usr/bin/env bash
# Configure only the signed ChimeraDB APT source. No database changes are made.

die() { printf 'ChimeraDB repository setup: %s\n' "$*" >&2; exit 1; }

usage() {
  cat <<'EOF'
usage: setup-repository.sh --key-fingerprint FULL_40_HEX [--repository-url HTTPS_URL]

Run as root on Debian 12 (bookworm) or Debian 13 (trixie), amd64.
Obtain the signing-key fingerprint independently from the release instructions.
This adds the repository and refreshes APT; it does not install ChimeraDB.
EOF
}

read_host() {
  local ID VERSION_ID VERSION_CODENAME
  [[ -r /etc/os-release ]] || die 'cannot identify this operating system'
  . /etc/os-release
  command -v dpkg >/dev/null || die 'dpkg is required; only Debian is supported'
  host_id=${ID:-}
  host_version=${VERSION_ID:-}
  host_suite=${VERSION_CODENAME:-}
  host_arch=$(dpkg --print-architecture)
  host_uid=$(id -u)
}

verify_key() {
  local key=$1 expected=$2 key_home=$3
  gpg --batch --no-options --no-autostart --homedir "$key_home" \
    --show-keys --with-colons "$key" |
    awk -F: -v expected="$expected" '
      $1 == "pub" { count++; pending=1; next }
      $1 == "sub" { pending=0; next }
      $1 == "fpr" && pending { fingerprint=$10; pending=0 }
      END {
        if (count != 1 || fingerprint != expected) {
          print "downloaded key must contain exactly the expected primary certificate" > "/dev/stderr"
          exit 1
        }
      }
    '
}

write_repository_files() {
  local key=$1 source=$2 key_path=$3 source_path=$4 path expected
  # Check both managed files before writing either. A different configuration
  # requires an explicit operator decision, including a signing-key rotation.
  for path in "$key_path" "$source_path"; do
    [[ ! -L $path ]] || die "refusing existing symlink: $path"
    if [[ -e $path ]]; then
      [[ -f $path ]] || die "not a regular managed file: $path"
      expected=$key
      [[ $path != "$source_path" ]] || expected=$source
      cmp -s "$expected" "$path" ||
        die "existing $path differs; inspect and remove it explicitly before rerunning"
    fi
  done
  install -d -m 0755 "$(dirname "$key_path")" "$(dirname "$source_path")"
  [[ -f $key_path ]] || install -m 0644 "$key" "$key_path"
  [[ -f $source_path ]] || install -m 0644 "$source" "$source_path"
  chmod 0644 "$key_path" "$source_path"
}

main() (
  set -euo pipefail
  local fingerprint= repository_url=https://mieweb.github.io/chimeraDB/apt
  local host_id host_version host_suite host_arch host_uid series work requirement
  while (($#)); do
    case $1 in
      --key-fingerprint)
        [[ $# -ge 2 && -n $2 ]] || die '--key-fingerprint requires a value'
        [[ -z $fingerprint ]] || die 'duplicate --key-fingerprint'
        fingerprint=$2; shift 2 ;;
      --repository-url)
        [[ $# -ge 2 && -n $2 ]] || die '--repository-url requires a value'
        repository_url=$2; shift 2 ;;
      -h|--help) usage; return 0 ;;
      *) die "unknown argument: $1" ;;
    esac
  done
  [[ $fingerprint =~ ^[0-9A-Fa-f]{40}$ ]] || die '--key-fingerprint must be a full 40-digit hexadecimal primary fingerprint'
  fingerprint=$(printf '%s' "$fingerprint" | tr 'a-f' 'A-F')
  # Keep the URL suitable for an APT URIs field. No credentials, whitespace,
  # query/fragment, non-HTTPS origin, or shell interpretation is accepted.
  [[ $repository_url =~ ^https://[A-Za-z0-9][A-Za-z0-9.-]*(:[0-9]+)?(/[-A-Za-z0-9._~%/]+)?$ ]] ||
    die '--repository-url must be an HTTPS base URL without credentials, query, or fragment'
  while [[ $repository_url == */ ]]; do repository_url=${repository_url%/}; done

  read_host
  case "$host_id:$host_version:$host_suite:$host_arch" in
    debian:12:bookworm:amd64) series=10.11 ;;
    debian:13:trixie:amd64) series=11.8 ;;
    *) die 'supported hosts are Debian 12 (bookworm) and Debian 13 (trixie), amd64 only' ;;
  esac
  [[ $host_uid == 0 ]] || die 'run this helper as root (for example, sudo bash setup-repository.sh ...)'
  for requirement in curl gpg; do
    command -v "$requirement" >/dev/null ||
      die 'install the prerequisites first: apt install ca-certificates curl gnupg'
  done

  work=$(mktemp -d)
  trap 'rm -rf -- "$work"' EXIT
  install -d -m 0700 "$work/gnupg"
  curl --fail --show-error --silent --location --retry 3 \
    --proto '=https' --proto-redir '=https' --tlsv1.2 \
    --output "$work/downloaded-key" "$repository_url/chimeradb-archive-keyring.gpg"
  verify_key "$work/downloaded-key" "$fingerprint" "$work/gnupg"
  gpg --batch --no-options --no-autostart --homedir "$work/gnupg" \
    --import-options import-minimal --import "$work/downloaded-key"
  gpg --batch --no-options --no-autostart --homedir "$work/gnupg" \
    --export-options export-minimal --export "$fingerprint" > "$work/chimeradb.gpg"
  verify_key "$work/chimeradb.gpg" "$fingerprint" "$work/gnupg"
  cat > "$work/chimeradb.sources" <<EOF
Types: deb
URIs: $repository_url
Suites: $host_suite
Components: main
Architectures: amd64
Signed-By: /etc/apt/keyrings/chimeradb.gpg
EOF
  write_repository_files "$work/chimeradb.gpg" "$work/chimeradb.sources" \
    /etc/apt/keyrings/chimeradb.gpg /etc/apt/sources.list.d/chimeradb.sources
  apt-get update --error-on=any
  printf '\nRepository ready for Debian %s / MariaDB %s. Next, run as root:\n\n' "$host_version" "$series"
  printf '%s\n' 'apt install chimeradb' 'systemctl restart mariadb' 'chimeradb setup' 'chimeradb status'
)

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then main "$@"; fi
