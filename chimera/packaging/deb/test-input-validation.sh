#!/usr/bin/env bash
# Server-free regression tests using real Debian archives and GnuPG keyrings.
# No APT, network, root privileges or running database is needed.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
for command in dpkg-deb sha256sum gpg gpgconf; do
  command -v "$command" >/dev/null || { echo "required: $command" >&2; exit 1; }
done
work=$(mktemp -d)
export GNUPGHOME="$work/gnupg"
mkdir -m 700 "$GNUPGHOME"
cleanup() {
  gpgconf --kill gpg-agent >/dev/null 2>&1 || true
  rm -rf "$work"
}
trap cleanup EXIT
checks=0
pass() { checks=$((checks + 1)); printf 'PASS: %s\n' "$1"; }
reject() {
  local label=$1
  shift
  if "$@" >"$work/rejected.log" 2>&1; then
    echo "FAIL: $label was accepted" >&2; exit 1
  fi
  pass "$label rejected"
}

make_package() {
  local destination=$1 name=$2 arch=$3 version=${4:-0.1.0-1} depends=${5:-}
  local tree
  tree=$(mktemp -d "$work/control.XXXXXX")
  mkdir "$tree/DEBIAN"
  cat >"$tree/DEBIAN/control" <<EOF
Package: $name
Version: $version
Architecture: $arch
Maintainer: ChimeraDB test <test@example.invalid>
Description: disposable package validation fixture
EOF
  [[ -z $depends ]] || printf 'Depends: %s\n' "$depends" >>"$tree/DEBIAN/control"
  dpkg-deb --build --root-owner-group "$tree" "$destination/${name}_${version}_${arch}.deb" >/dev/null
}
manifest() { (cd "$1" && sha256sum ./*.deb >SHA256SUMS); }
verify() { "$HERE/verify-packages.sh" "$1" 11.8 amd64 'Debian 12 (bookworm)' "${@:2}"; }
base="$work/base"
mkdir "$base"
make_package "$base" chimeradb all
make_package "$base" chimeradb-common all
make_package "$base" chimeradb-plugin-11.8 amd64 0.1.0-1 'mariadb-server (= 1:11.8.9+maria~deb12)'
cat >"$base/build-info.txt" <<'EOF'
ChimeraDB: 0.1.0-1
MariaDB package: 1:11.8.9+maria~deb12
MariaDB series: 11.8
Architecture: amd64
Distribution: Debian 12 (bookworm)
EOF
manifest "$base"
[[ $(verify "$base") == 0.1.0-1 ]]
pass 'complete three-package input'
cp -R "$base" "$work/debug"
make_package "$work/debug" chimeradb-plugin-11.8-dbgsym amd64
manifest "$work/debug"
runtime=$(verify "$work/debug" --runtime-files)
[[ $(printf '%s\n' "$runtime" | wc -l) == 3 && $runtime != *-dbgsym_* ]]
pass 'debug artifact verified but excluded from the three runtime inputs'

cp -R "$base" "$work/unlisted"
cp "$work/debug/"*dbgsym*.deb "$work/unlisted/"
reject 'unlisted package despite otherwise valid SHA256SUMS' verify "$work/unlisted"
cp -R "$base" "$work/tampered"
printf changed >>"$work/tampered/chimeradb_0.1.0-1_all.deb"
reject 'modified package bytes' verify "$work/tampered"
cp -R "$base" "$work/foreign"
make_package "$work/foreign" unrelated-package all
manifest "$work/foreign"
reject 'checksummed foreign package' verify "$work/foreign"
cp -R "$base" "$work/duplicate"
cp "$work/duplicate/chimeradb-common_0.1.0-1_all.deb" "$work/duplicate/duplicate.deb"
manifest "$work/duplicate"
reject 'duplicate package identity under another filename' verify "$work/duplicate"
cp -R "$work/debug" "$work/missing"
rm "$work/missing/chimeradb-common_0.1.0-1_all.deb"
manifest "$work/missing"
reject 'missing runtime package with debug artifact present' verify "$work/missing"
cp -R "$base" "$work/arch"
rm "$work/arch/chimeradb-plugin-11.8_0.1.0-1_amd64.deb"
make_package "$work/arch" chimeradb-plugin-11.8 arm64 0.1.0-1 'mariadb-server (= 1:11.8.9+maria~deb12)'
manifest "$work/arch"
reject 'wrong plugin architecture' verify "$work/arch"
cp -R "$base" "$work/version"
rm "$work/version/chimeradb-common_0.1.0-1_all.deb"
make_package "$work/version" chimeradb-common all 0.1.0-2
manifest "$work/version"
reject 'mixed package versions' verify "$work/version"
cp -R "$base" "$work/server"
make_package "$work/server" chimeradb-plugin-11.8 amd64 0.1.0-1 'mariadb-server (= 1:11.8.8+maria~deb12)'
manifest "$work/server"
reject 'wrong exact MariaDB ABI dependency' verify "$work/server"
reject 'wrong distribution' "$HERE/verify-packages.sh" "$base" 11.8 amd64 'Debian 13 (trixie)'
reject 'wrong series' "$HERE/verify-packages.sh" "$base" 10.11 amd64 'Debian 12 (bookworm)'
cp -R "$base" "$work/manifest-duplicate"
head -n 1 "$base/SHA256SUMS" >>"$work/manifest-duplicate/SHA256SUMS"
reject 'duplicate manifest record' verify "$work/manifest-duplicate"
cp -R "$base" "$work/manifest-path"
printf '%064d  ../outside.deb\n' 0 >>"$work/manifest-path/SHA256SUMS"
reject 'out-of-directory manifest entry' verify "$work/manifest-path"

# Exercise the native harness's actual two package functions without sourcing
# its destructive top-level host/service workflow. APT is a recording function;
# no installation, root privilege, systemd, host path or repository is touched.
sed -n '/^validate_packages() {$/,/^}$/p; /^install_packages() {$/,/^}$/p' \
  "$HERE/systemd-test.sh" >"$work/native-functions.sh"
[[ -s $work/native-functions.sh ]]
source "$work/native-functions.sh"
declare -F validate_packages >/dev/null
declare -F install_packages >/dev/null
series=11.8 arch=amd64 distribution='Debian 12 (bookworm)'
apt_options=(-y)
apt-get() { printf '%s\n' "$@" >"$work/native-apt-arguments"; }
install_packages "$work/debug" --reinstall
expected_runtime=$(verify "$work/debug" --runtime-files)
expected_arguments=$(printf '%s\n' -y install --no-install-recommends --reinstall "$expected_runtime")
[[ $(cat "$work/native-apt-arguments") == "$expected_arguments" ]]
pass 'native installation delegates validation and installs exactly three runtime packages'
rm "$work/native-apt-arguments"

cp -R "$base" "$work/symlink"
rm "$work/symlink/chimeradb-common_0.1.0-1_all.deb"
ln -s "$base/chimeradb-common_0.1.0-1_all.deb" "$work/symlink/chimeradb-common_0.1.0-1_all.deb"
manifest "$work/symlink"
cp -R "$work/debug" "$work/duplicate-debug"
cp "$work/duplicate-debug/"*dbgsym*.deb "$work/duplicate-debug/another-debug.deb"
manifest "$work/duplicate-debug"
cp -R "$base" "$work/alternative-dependency"
make_package "$work/alternative-dependency" chimeradb-plugin-11.8 amd64 0.1.0-1 \
  'mariadb-server (= 1:11.8.9+maria~deb12) | unrelated-server'
manifest "$work/alternative-dependency"
for fixture in symlink duplicate-debug alternative-dependency unlisted missing; do
  reject "native validator $fixture input" validate_packages "$work/$fixture"
  reject "native installation $fixture input" install_packages "$work/$fixture"
  [[ ! -e $work/native-apt-arguments ]] || { echo 'FAIL: rejected native input reached APT' >&2; exit 1; }
done
unset -f apt-get

# The keyring tests exercise GnuPG parsing, including a real bound subkey; a
# colon-output fixture alone would not verify the export/show-keys interface.
gpg --batch --pinentry-mode loopback --passphrase '' --quick-generate-key \
  'Chimera pinned fixture <pinned@example.invalid>' rsa2048 sign 0 >"$work/gpg.log" 2>&1
primary=$(gpg --batch --with-colons --list-keys 2>/dev/null | awk -F: '$1 == "fpr" { print $10; exit }')
gpg --batch --pinentry-mode loopback --passphrase '' --quick-add-key "$primary" \
  rsa2048 sign 0 >>"$work/gpg.log" 2>&1
gpg --batch --export "$primary" >"$work/pinned.gpg"
"$HERE/verify-keyring.sh" "$work/pinned.gpg" "$primary"
pass 'sole pinned primary key with its bound subkey'
env HOME="$work/missing-home" GNUPGHOME="$work/missing-gnupg" \
  "$HERE/verify-keyring.sh" "$work/pinned.gpg" "$primary"
[[ ! -e $work/missing-home && ! -e $work/missing-gnupg ]]
pass 'clean environment needs no caller GnuPG home and leaves it untouched'
subkey=$(gpg --batch --with-colons --with-subkey-fingerprint --list-keys "$primary" 2>/dev/null |
  awk -F: '$1 == "sub" { subkey=1 } $1 == "fpr" && subkey { print $10; exit }')
reject 'fingerprint present only as a subkey' "$HERE/verify-keyring.sh" "$work/pinned.gpg" "$subkey"
gpg --batch --pinentry-mode loopback --passphrase '' --quick-generate-key \
  'Chimera unrelated fixture <unrelated@example.invalid>' rsa2048 sign 0 >>"$work/gpg.log" 2>&1
gpg --batch --export >"$work/bundle.gpg"
reject 'bundle containing pinned and unrelated primary keys' "$HERE/verify-keyring.sh" "$work/bundle.gpg" "$primary"
gpg --batch --export 'unrelated@example.invalid' >"$work/wrong.gpg"
reject 'unrelated sole primary key' "$HERE/verify-keyring.sh" "$work/wrong.gpg" "$primary"
: >"$work/empty.gpg"
reject 'unreadable keyring' "$HERE/verify-keyring.sh" "$work/empty.gpg" "$primary"
"$HERE/extract-keyring.sh" "$work/bundle.gpg" "$work/filtered.gpg" "$primary" >>"$work/gpg.log" 2>&1
"$HERE/verify-keyring.sh" "$work/filtered.gpg" "$primary"
pass 'extraction removes unrelated primary keys from downloaded bundle'
cp "$work/filtered.gpg" "$work/previous-filtered.gpg"
reject 'extraction without pinned primary' "$HERE/extract-keyring.sh" "$work/wrong.gpg" "$work/filtered.gpg" "$primary"
cmp "$work/filtered.gpg" "$work/previous-filtered.gpg"
reject 'extraction pin matching only a subkey' "$HERE/extract-keyring.sh" "$work/pinned.gpg" "$work/filtered.gpg" "$subkey"
cmp "$work/filtered.gpg" "$work/previous-filtered.gpg"
env HOME="$work/missing-home" GNUPGHOME="$work/missing-gnupg" \
  "$HERE/extract-keyring.sh" "$work/bundle.gpg" "$work/clean-filtered.gpg" "$primary" >>"$work/gpg.log" 2>&1
[[ ! -e $work/missing-home && ! -e $work/missing-gnupg ]]
"$HERE/verify-keyring.sh" "$work/clean-filtered.gpg" "$primary"
pass 'extraction isolates GnuPG state in a clean environment'
printf 'PASS: %s package/keyring input validation checks\n' "$checks"
