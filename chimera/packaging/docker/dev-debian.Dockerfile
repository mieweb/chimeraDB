# The Linux half of the dev path (M9.0.2).
#
# Everything chimera/scripts needs to build a MariaDB source tree, the
# translator, and the plugin on Debian. Not a shipping artifact: no ChimeraDB
# source is copied in, because the point is to run the *checkout* — bind-mounted
# by dev.sh — through the same scripts a developer runs on macOS.
ARG SUITE=bookworm
FROM debian:${SUITE}
ARG SUITE

# Debian's own mariadb-server build-dependency list is authoritative; writing
# one out by hand here would rot the first time the server needs a new library.
RUN set -eux; \
    printf 'deb-src http://deb.debian.org/debian %s main\n' "${SUITE}" \
      > /etc/apt/sources.list.d/chimera-src.list; \
    apt-get update; \
    apt-get build-dep -y mariadb-server; \
    apt-get install -y --no-install-recommends \
      ca-certificates cmake ninja-build pkg-config procps \
      libbson-dev libssl-dev doctest-dev; \
    rm -rf /var/lib/apt/lists/*

# Out-of-tree, so the bind-mounted checkout stays exactly as the host left it.
ENV CHIMERA_OUT=/out
WORKDIR /work
CMD ["bash"]
