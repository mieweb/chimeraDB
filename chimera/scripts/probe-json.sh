#!/usr/bin/env bash
# M0.8 — assert the JSON features the whole storage model depends on (D3, D5).
#
#   probe-json.sh --server 10.11|11.8
#
# Run against a live instance started by run-server.sh. Fails loudly if either
# JSON_VALUE path extraction or generated columns over a JSON column regress.

source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

chimera_parse_server "$@"
((${#CHIMERA_ARGS[@]} == 0)) || die "unknown argument '${CHIMERA_ARGS[0]}'"
chimera_require_running

note "JSON_VALUE nested path extraction"
got=$(chimera_sql -N -B -e "SELECT JSON_VALUE('{\"a\":{\"b\":2}}','\$.a.b');")
[[ $got == 2 ]] || die "expected 2, got '$got'"

note "generated VIRTUAL column over a JSON column"
got=$(chimera_sql -N -B -e "
  CREATE DATABASE IF NOT EXISTS chimera_probe;
  USE chimera_probe;
  CREATE TEMPORARY TABLE t (d JSON, v INT AS (JSON_VALUE(d,'\$.x')) VIRTUAL);
  INSERT INTO t (d) VALUES ('{\"x\": 41}');
  SELECT v FROM t;
  DROP DATABASE chimera_probe;")
[[ $got == 41 ]] || die "expected 41, got '$got'"

note "JSON probe passed on $SERVER_VERSION"
