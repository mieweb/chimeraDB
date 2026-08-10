-- chimera catalog — the metadata ChimeraDB keeps about collections it manages.
--
-- Deliberately tiny: MariaDB's own information_schema already knows the tables,
-- columns, and indexes. The only thing it cannot know is which tables are
-- ChimeraDB collections and how their projections should be maintained (D4).
--
-- Idempotent: safe to re-apply against a live server.

CREATE DATABASE IF NOT EXISTS chimera_meta;

CREATE TABLE IF NOT EXISTS chimera_meta.collections (
  db_name         VARCHAR(64) NOT NULL COMMENT 'MariaDB database = Mongo database',
  coll_name       VARCHAR(64) NOT NULL COMMENT 'MariaDB table = Mongo collection',
  projection_mode ENUM('manual','eager','lazy') NOT NULL DEFAULT 'manual',
  created_at      TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (db_name, coll_name)
) ENGINE=InnoDB;
