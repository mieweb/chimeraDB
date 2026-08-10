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

-- Mongo index definitions. information_schema knows the MariaDB index that was
-- created, but not which document path each column came from or what the client
-- named it, and listIndexes has to answer both.
CREATE TABLE IF NOT EXISTS chimera_meta.indexes (
  db_name    VARCHAR(64)  NOT NULL,
  coll_name  VARCHAR(64)  NOT NULL,
  index_name VARCHAR(128) NOT NULL COMMENT 'the name the mongo client chose',
  key_spec   JSON         NOT NULL COMMENT 'the {path: 1|-1} document, verbatim',
  is_unique  TINYINT(1)   NOT NULL DEFAULT 0,
  PRIMARY KEY (db_name, coll_name, index_name)
) ENGINE=InnoDB;
