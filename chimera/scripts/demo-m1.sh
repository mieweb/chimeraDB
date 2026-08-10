#!/usr/bin/env bash
# M1 — prove the ChimeraDB storage model with nothing but SQL.
#
#   demo-m1.sh --server 10.11|11.8
#
# Walks the whole doc-store contract and asserts every step, so a regression on
# either LTS line fails loudly:
#   1  a collection is an InnoDB table (_id PK + doc JSON) with a catalog row
#   2  documents are canonical Extended JSON (D5)
#   3  a forward projection is one ALTER, and the ALTER *is* the backfill (D3)
#   4  a VIRTUAL projection costs no rebuild
#   5  forward projections cannot drift — the engine refuses the write
#   6  type mismatches follow sql_mode, not a ChimeraDB policy (D8)
#   7  a bidirectional column writes through to the document (D10)
#
# Destroys and rebuilds the `test` database each run.

source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

chimera_parse_server "$@"
((${#CHIMERA_ARGS[@]} == 0)) || die "unknown argument '${CHIMERA_ARGS[0]}'"
chimera_require_running

DOC1='{"_id":"u1","name":"Doug","email":"doug@example.com","createdAt":{"$date":{"$numberLong":"1786694400000"}}}'
DOC2='{"_id":"u2","name":"Ada","email":"ada@example.com","age":30}'

note "1. catalog + collection table"
chimera_sql < "$CHIMERA_DIR/sql/catalog.sql"
chimera_sql -e "
  DROP DATABASE IF EXISTS test;
  CREATE DATABASE test;
  CREATE TABLE test.users (
    _id VARBINARY(255) NOT NULL PRIMARY KEY,
    doc JSON NOT NULL
  ) ENGINE=InnoDB;
  REPLACE INTO chimera_meta.collections (db_name, coll_name) VALUES ('test','users');"
check_eq "projection_mode default" \
  "$(sql_scalar "SELECT projection_mode FROM chimera_meta.collections WHERE db_name='test' AND coll_name='users'")" \
  "manual"

note "2. insert two canonical extJSON documents"
chimera_sql -e "INSERT INTO test.users (_id, doc) VALUES ('u1', '$DOC1'), ('u2', '$DOC2');"
check_eq "row count" "$(sql_scalar "SELECT COUNT(*) FROM test.users")" "2"
# The extJSON type wrapper is just more JSON — reachable with a quoted path member.
check_eq "extJSON \$date path" \
  "$(sql_scalar "SELECT JSON_VALUE(doc,'\$.createdAt.\"\$date\".\"\$numberLong\"') FROM test.users WHERE _id='u1'")" \
  "1786694400000"

note "3. update a document with JSON_SET"
chimera_sql -e "UPDATE test.users SET doc = JSON_SET(doc, '\$.age', 31) WHERE _id = 'u2';"
check_eq "age after JSON_SET" "$(sql_scalar "SELECT JSON_VALUE(doc,'\$.age') FROM test.users WHERE _id='u2'")" "31"

note "4. forward projection: one ALTER, and the rebuild IS the backfill (D3)"
# PERSISTENT cannot be added instantly *because* every existing row must be
# materialized — that refusal is the proof the backfill happens during the ALTER.
check_sql_error "PERSISTENT projection with ALGORITHM=INSTANT" 1845 \
  "ALTER TABLE test.users ADD COLUMN email VARCHAR(190) AS (JSON_VALUE(doc,'\$.email')) PERSISTENT, ALGORITHM=INSTANT;"
chimera_sql -e "
  ALTER TABLE test.users
    ADD COLUMN email VARCHAR(190) AS (JSON_VALUE(doc,'\$.email')) PERSISTENT,
    ADD INDEX (email);"
check_eq "backfilled email" "$(sql_scalar "SELECT email FROM test.users WHERE _id='u1'")" "doug@example.com"
check_eq "email index used" \
  "$(sql_scalar "EXPLAIN SELECT _id FROM test.users WHERE email='ada@example.com'" | awk '{print $6}')" \
  "email"

note "5. VIRTUAL projection: instant, no rebuild"
chimera_sql -e "
  ALTER TABLE test.users
    ADD COLUMN created_ms BIGINT AS (JSON_VALUE(doc,'\$.createdAt.\"\$date\".\"\$numberLong\"')) VIRTUAL,
    ALGORITHM=INSTANT;
  ALTER TABLE test.users ADD INDEX (created_ms);"
check_eq "virtual created_ms" "$(sql_scalar "SELECT created_ms FROM test.users WHERE _id='u1'")" "1786694400000"

note "6. drift is impossible: the engine refuses to write a forward projection"
check_sql_error "direct write to generated column" 1906 "UPDATE test.users SET email='x' WHERE _id='u1';"

note "7. type mismatch follows sql_mode (D8)"
# Default (strict) sql_mode: the ALTER itself refuses to materialize a bad value.
check_sql_error "INT projection over a string path, strict" 1366 \
  "ALTER TABLE test.users ADD COLUMN bad_age INT AS (JSON_VALUE(doc,'\$.name')) PERSISTENT;"
# Permissive sql_mode: warning + coerced value, no error.
warning=$(chimera_sql -N -B -e "
  SET SESSION sql_mode='';
  ALTER TABLE test.users ADD COLUMN bad_age INT AS (JSON_VALUE(doc,'\$.name')) PERSISTENT;
  SHOW WARNINGS;" | head -1 | cut -f2)
check_eq "permissive warning code" "$warning" "1366"
check_eq "coerced value (not NULL)" "$(sql_scalar "SELECT bad_age FROM test.users WHERE _id='u1'")" "0"
chimera_sql -e "ALTER TABLE test.users DROP COLUMN bad_age;"

note "8. bidirectional projection: plain SQL writes through to the document (D10)"
chimera_sql -e "
  ALTER TABLE test.users ADD COLUMN name VARCHAR(190) NULL, ADD INDEX (name);
  UPDATE test.users SET name = JSON_VALUE(doc,'\$.name');"
chimera_sql <<'SQL'
USE test;
DELIMITER $$
CREATE TRIGGER users_name_bi BEFORE INSERT ON test.users FOR EACH ROW
  SET NEW.name = JSON_VALUE(NEW.doc, '$.name')$$
-- Doc wins when one statement changes both sides; otherwise the column change
-- is written through into the document, which stays the source of truth.
CREATE TRIGGER users_name_bu BEFORE UPDATE ON test.users FOR EACH ROW
BEGIN
  IF NOT (NEW.doc <=> OLD.doc) THEN
    SET NEW.name = JSON_VALUE(NEW.doc, '$.name');
  ELSEIF NOT (NEW.name <=> OLD.name) THEN
    SET NEW.doc = JSON_SET(NEW.doc, '$.name', NEW.name);
  END IF;
END$$
DELIMITER ;
SQL

# The README's headline example, verbatim.
chimera_sql -e "UPDATE test.users SET name = 'Douglas Horner' WHERE email = 'doug@example.com';"
check_eq "write-through reached the document" \
  "$(sql_scalar "SELECT JSON_VALUE(doc,'\$.name') FROM test.users WHERE _id='u1'")" "Douglas Horner"

chimera_sql -e "UPDATE test.users SET doc = JSON_SET(doc,'\$.name','Doc Wins') WHERE _id='u1';"
check_eq "doc change refreshes the column" "$(sql_scalar "SELECT name FROM test.users WHERE _id='u1'")" "Doc Wins"

chimera_sql -e "UPDATE test.users SET doc = JSON_SET(doc,'\$.name','FromDoc'), name = 'FromCol' WHERE _id='u1';"
check_eq "doc wins when both change" "$(sql_scalar "SELECT name FROM test.users WHERE _id='u1'")" "FromDoc"

chimera_sql -e "INSERT INTO test.users (_id, doc) VALUES ('u3', '{\"_id\":\"u3\",\"name\":\"Grace\"}');"
check_eq "insert populates the column" "$(sql_scalar "SELECT name FROM test.users WHERE _id='u3'")" "Grace"

note "demo-m1 passed on $SERVER_VERSION"
