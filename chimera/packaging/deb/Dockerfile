# syntax=docker/dockerfile:1
# Build with the repository root as context. BuildKit only receives chimera/.
ARG DEBIAN_SUITE=bookworm
FROM debian:${DEBIAN_SUITE} AS repository
ARG SERIES=10.11
ENV DEBIAN_FRONTEND=noninteractive
COPY chimera/packaging/deb/configure-repository.sh /usr/local/bin/chimera-configure-repository
RUN /usr/local/bin/chimera-configure-repository "$SERIES"

FROM repository AS builder
ARG SERIES=10.11
ARG MARIADB_VERSION=
ARG REVISION=1
ARG SOURCE_DATE_EPOCH=1786320000
ARG JOBS=4
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential debhelper dpkg-dev cmake ninja-build pkg-config \
      libbson-dev doctest-dev && \
    apt-get build-dep -y mariadb-server && \
    rm -rf /var/lib/apt/lists/*
WORKDIR /work
COPY chimera/ /work/chimera/
RUN SERIES="$SERIES" MARIADB_VERSION="$MARIADB_VERSION" REVISION="$REVISION" \
    SOURCE_DATE_EPOCH="$SOURCE_DATE_EPOCH" JOBS="$JOBS" \
    /work/chimera/packaging/deb/build-inside.sh

FROM scratch AS packages
COPY --from=builder /packages/ /
