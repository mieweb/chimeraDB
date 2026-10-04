# syntax=docker/dockerfile:1
# build.sh supplies a small context with the matching Debian packages only.
FROM debian:bookworm-slim
ARG SERIES=10.11
ENV DEBIAN_FRONTEND=noninteractive
COPY configure-repository.sh /usr/local/bin/chimera-configure-repository
COPY packages/ /tmp/packages/
RUN printf '#!/bin/sh\nexit 101\n' > /usr/sbin/policy-rc.d && \
    chmod 755 /usr/sbin/policy-rc.d && \
    /usr/local/bin/chimera-configure-repository "$SERIES" && \
    cd /tmp/packages && sha256sum --check SHA256SUMS && \
    apt-get install -y --no-install-recommends ./*.deb gosu && \
    rm -rf /var/lib/apt/lists/* /tmp/packages /var/lib/mysql && \
    mkdir -p /var/lib/mysql /run/mysqld && \
    chown mysql:mysql /var/lib/mysql /run/mysqld
COPY 70-chimera-container.cnf /etc/mysql/mariadb.conf.d/70-chimera-container.cnf
COPY entrypoint.sh /usr/local/bin/chimera-entrypoint
RUN chmod 755 /usr/local/bin/chimera-entrypoint
EXPOSE 3306 27017
VOLUME ["/var/lib/mysql"]
HEALTHCHECK --interval=10s --timeout=5s --start-period=60s --retries=6 \
  CMD test ! -e /var/lib/mysql/.chimera-initializing && chimeradb status --protocol=socket --socket=/run/mysqld/mysqld.sock --user=root >/dev/null || exit 1
ENTRYPOINT ["chimera-entrypoint"]
CMD ["mariadbd"]
