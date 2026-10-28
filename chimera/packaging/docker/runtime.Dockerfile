# syntax=docker/dockerfile:1
# build.sh supplies a small context with the matching Debian packages only.
FROM debian:bookworm-slim AS verified-packages
ARG SERIES=10.11
COPY verify-packages.sh /usr/local/bin/chimera-verify-packages
COPY packages/ /tmp/packages/
RUN chimera-verify-packages /tmp/packages "$SERIES" "$(dpkg --print-architecture)" \
      'Debian 12 (bookworm)' --runtime-files > /tmp/runtime-files && \
    mkdir /runtime-packages && \
    while IFS= read -r package; do cp "$package" /runtime-packages/; done < /tmp/runtime-files

FROM debian:bookworm-slim
ARG SERIES=10.11
ENV DEBIAN_FRONTEND=noninteractive
COPY configure-repository.sh /usr/local/bin/chimera-configure-repository
COPY verify-keyring.sh /usr/local/bin/verify-keyring.sh
COPY extract-keyring.sh /usr/local/bin/extract-keyring.sh
# Only the verified three runtime packages enter the final image. Debug symbols
# remain downloadable artifacts and do not occupy any layer of the runtime.
COPY --from=verified-packages /runtime-packages/ /tmp/packages/
RUN printf '#!/bin/sh\nexit 101\n' > /usr/sbin/policy-rc.d && \
    chmod 755 /usr/sbin/policy-rc.d && \
    /usr/local/bin/chimera-configure-repository "$SERIES" && \
    cd /tmp/packages && \
    apt-get install -y --no-install-recommends ./*.deb gosu && \
    rm -rf /var/lib/apt/lists/* /tmp/packages /var/lib/mysql && \
    mkdir -p /var/lib/mysql /run/mysqld && \
    chown mysql:mysql /var/lib/mysql /run/mysqld
COPY 70-chimera-container.cnf /etc/mysql/mariadb.conf.d/70-chimera-container.cnf
COPY entrypoint.sh /usr/local/bin/chimera-entrypoint
RUN chmod 755 /usr/local/bin/chimera-entrypoint
EXPOSE 3306 27017
VOLUME ["/var/lib/mysql"]
# chimeradb status requires a bounded successful Mongo ping, not just plugin ACTIVE.
HEALTHCHECK --interval=10s --timeout=5s --start-period=60s --retries=6 \
  CMD test ! -e /var/lib/mysql/.chimera-initializing && chimeradb status --protocol=socket --socket=/run/mysqld/mysqld.sock --user=root >/dev/null || exit 1
ENTRYPOINT ["chimera-entrypoint"]
CMD ["mariadbd"]
