#!/usr/bin/env bash
# Internal builder entry point. Run build.sh on the host.
set -euo pipefail
series=${SERIES:?}
jobs=${JOBS:-4}
export CHIMERA_OUT=/build
export CMAKE_BUILD_PARALLEL_LEVEL=$jobs
export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:?}
export DEB_BUILD_MAINT_OPTIONS=hardening=+all
eval "$(dpkg-buildflags --export=sh)"
export CFLAGS="$CFLAGS $CPPFLAGS" CXXFLAGS="$CXXFLAGS $CPPFLAGS"
apt-get update
version=${MARIADB_VERSION:-$(apt-cache policy mariadb-server | awk '/Candidate:/ {print $2}')}
upstream=${version#*:}
[[ $upstream == "$series".* ]] || {
  echo "MariaDB candidate $version does not match requested series $series" >&2; exit 1;
}
mkdir -p /source /packages
cd /source
# APT verifies the signed repository and source checksums. Building the exact
# binary package's source is necessary because mongo() accesses internal THD.
apt-get source "mariadb-server=$version"
source_tree=$(find /source -mindepth 2 -maxdepth 2 -name CMakeLists.txt -print)
[[ $(printf '%s\n' "$source_tree" | wc -l) == 1 && -f $source_tree ]] || {
  echo 'Expected exactly one MariaDB source tree' >&2; exit 1;
}
source_tree=${source_tree%/CMakeLists.txt}
ln -s /work/chimera/plugin/chimera_mongo "$source_tree/plugin/chimera_mongo"
/work/chimera/scripts/build-translator.sh
# Debian's patched server source declares duplicate static client archive
# outputs under Ninja. Use the supported Make generator without editing any
# upstream source; only the plugin target is built either way.
cmake -S "$source_tree" -B /build/server -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_CONFIG=mysql_release -DDEB=Debian \
  -DWITH_UNIT_TESTS=OFF -DPLUGIN_ROCKSDB=NO -DPLUGIN_MROONGA=NO \
  -DPLUGIN_SPIDER=NO -DPLUGIN_OQGRAPH=NO -DPLUGIN_SPHINX=NO \
  -DPLUGIN_CONNECT=NO -DPLUGIN_COLUMNSTORE=NO -DPLUGIN_TOKUDB=NO
cmake --build /build/server --target chimera_mongo --parallel "$jobs"
plugin=/build/server/plugin/chimera_mongo/chimera_mongo.so
test -f "$plugin"

cd /work
cp -R chimera/packaging/deb/debian ./debian
sed "s/@SERIES@/$series/g" debian/control.in > debian/control
rm debian/control.in
chmod 755 debian/rules
release=$(cat chimera/VERSION)
export CHIMERA_SERIES=$series CHIMERA_SERVER_VERSION=$version CHIMERA_PLUGIN=$plugin
cat > debian/changelog <<EOF
chimeradb ($release-${REVISION:-1}) unstable; urgency=medium

  * Package ChimeraDB for the supported MariaDB series on Debian 12.

 -- ChimeraDB contributors <support@mieweb.com>  $(date --utc --date="@$SOURCE_DATE_EPOCH" -R)
EOF
cp debian/plugin.postinst "debian/chimeradb-plugin-$series.postinst"
cp debian/plugin.postrm "debian/chimeradb-plugin-$series.postrm"
dpkg-buildpackage --build=binary --no-sign
cp /chimeradb*.deb /packages/
cat > /packages/build-info.txt <<EOF
ChimeraDB: $release-${REVISION:-1}
MariaDB package: $version
MariaDB series: $series
Architecture: $(dpkg --print-architecture)
Distribution: Debian 12 (bookworm)
SOURCE_DATE_EPOCH: $SOURCE_DATE_EPOCH
EOF
cd /packages
sha256sum ./*.deb > SHA256SUMS
