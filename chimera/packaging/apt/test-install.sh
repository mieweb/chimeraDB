#!/usr/bin/env bash
# Test public-repository installation in a disposable container, never the host.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repository= repository_url= fingerprint= expected_version= suite=
while (($#)); do
  case $1 in
    --repository) repository=${2:?missing repository directory}; shift 2 ;;
    --repository-url) repository_url=${2:?missing repository URL}; shift 2 ;;
    --key-fingerprint) fingerprint=${2:?missing key fingerprint}; shift 2 ;;
    --expected-version) expected_version=${2:?missing package version}; shift 2 ;;
    --suite) suite=${2:?missing Debian suite}; shift 2 ;;
    *) echo 'usage: test-install.sh (--repository DIR | --repository-url HTTPS_URL --key-fingerprint FINGERPRINT) --suite bookworm|trixie' >&2; exit 1 ;;
  esac
done
[[ $suite == bookworm || $suite == trixie ]]
mounts=()
if [[ -n $repository ]]; then
  [[ -z $repository_url && -z $fingerprint ]]
  repository=$(cd "$repository" && pwd)
  [[ -f $repository/dists/$suite/InRelease && -f $repository/chimeradb-archive-keyring.gpg ]]
  mounts=(--mount "type=bind,src=$repository,dst=/repository,readonly")
else
  [[ $repository_url == https://* && $fingerprint =~ ^[A-Fa-f0-9]{40}$ ]]
fi
docker run --rm --platform linux/amd64 \
  "${mounts[@]}" \
  --env "CHIMERA_APT_URL=$repository_url" --env "CHIMERA_APT_KEY=$fingerprint" \
  --env "CHIMERA_EXPECTED_VERSION=$expected_version" \
  --mount "type=bind,src=$HERE/setup-repository.sh,dst=/setup-repository.sh,readonly" \
  --mount "type=bind,src=$HERE/test-install-inside.sh,dst=/test-install.sh,readonly" \
  "debian:$suite" bash /test-install.sh
