-- Private SCRAM verifiers, separate from chimera_meta (the oplog definer has
-- grants on chimera_meta.*). Only SQL administrators may read or change this.
CREATE DATABASE IF NOT EXISTS chimera_auth;
CREATE TABLE IF NOT EXISTS chimera_auth.credentials (
  auth_db VARBINARY(64) NOT NULL,
  user_name VARBINARY(128) NOT NULL,
  sql_host VARBINARY(255) NOT NULL,
  salt VARBINARY(64) NOT NULL,
  iterations INT UNSIGNED NOT NULL CHECK (iterations BETWEEN 4096 AND 1000000),
  stored_key BINARY(32) NOT NULL,
  server_key BINARY(32) NOT NULL,
  revision BINARY(16) NOT NULL,
  PRIMARY KEY (auth_db, user_name)
) ENGINE=InnoDB;
