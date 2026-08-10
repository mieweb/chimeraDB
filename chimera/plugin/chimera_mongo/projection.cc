// Projections: making one document field visible to SQL as an ordinary column.
//
// `forward` is a GENERATED column — SQL can read and index the field, and the
// document remains its only writer. `bidirectional` adds a real column plus a
// pair of BEFORE triggers that reconcile the two representations, so a plain
// `UPDATE … SET qty = 5` rewrites the document as well.
//
// The BEFORE/AFTER split is what makes this compose with the oplog:
// reconciliation finishes before the oplog trigger runs, so the entry it writes
// already carries the merged document — one 'u' entry, not two.
#include "projection.h"

namespace chimera {
namespace {

const char kProjectionsTable[] =
    "CREATE TABLE IF NOT EXISTS chimera_meta.projections ("
    " db_name VARCHAR(64) NOT NULL,"
    " coll_name VARCHAR(64) NOT NULL,"
    " column_name VARCHAR(64) NOT NULL,"
    " field VARCHAR(64) NOT NULL,"
    " mode ENUM('forward','bidirectional') NOT NULL,"
    " PRIMARY KEY (db_name, coll_name, column_name)) ENGINE=InnoDB";

// The two generated fragments are built once with `~DOC~` and `~COL~` standing
// in for whatever they attach to, then substituted per use site — a generated
// column says `doc`, a trigger says `NEW.doc`. The tokens cannot occur in a
// whitelisted identifier, so the substitution is exact.
const char kAddProjection[] =
    "CREATE OR REPLACE DEFINER='chimera'@'localhost'"
    " PROCEDURE chimera_meta.chimera_add_projection("
    " IN p_ns VARCHAR(129), IN p_field VARCHAR(64), IN p_column VARCHAR(64),"
    " IN p_type VARCHAR(64), IN p_mode VARCHAR(16))"
    " MODIFIES SQL DATA"
    " SQL SECURITY INVOKER"
    " COMMENT 'Expose a document field as a column: forward or bidirectional'"
    " BEGIN"
    "  DECLARE v_db VARCHAR(64);"
    "  DECLARE v_coll VARCHAR(64);"
    "  DECLARE v_tbl VARCHAR(200);"
    "  DECLARE v_col VARCHAR(70);"
    "  DECLARE v_path VARCHAR(80);"
    "  DECLARE v_read LONGTEXT;"
    "  DECLARE v_patch LONGTEXT;"
    "  DECLARE v_stem VARCHAR(40);"

    // Identifiers are concatenated into DDL, so they are whitelisted first. The
    // field must be a single top-level name: a nested path would need a JSON
    // patch this procedure does not build, and projecting the wrong thing
    // silently is worse than refusing.
    "  IF p_ns NOT REGEXP '^[A-Za-z0-9_$-]+[.][A-Za-z0-9_$.-]+$'"
    "   OR p_field NOT REGEXP '^[A-Za-z0-9_$-]+$'"
    "   OR p_column NOT REGEXP '^[A-Za-z0-9_$]+$'"
    "   OR p_type NOT REGEXP '^[A-Za-z0-9_ ,()]+$' THEN"
    "   SIGNAL SQLSTATE '45000'"
    "    SET MESSAGE_TEXT = 'chimera_add_projection: unsupported identifier';"
    "  END IF;"
    "  IF p_mode NOT IN ('forward', 'bidirectional') THEN"
    "   SIGNAL SQLSTATE '45000'"
    "    SET MESSAGE_TEXT = 'chimera_add_projection: mode must be forward or bidirectional';"
    "  END IF;"

    "  SET v_db = SUBSTRING_INDEX(p_ns, '.', 1);"
    "  SET v_coll = SUBSTRING(p_ns FROM CHAR_LENGTH(v_db) + 2);"
    "  SET v_tbl = CONCAT('`', v_db, '`.`', v_coll, '`');"
    "  SET v_col = CONCAT('`', p_column, '`');"
    "  SET v_path = CONCAT('$.\"', p_field, '\"');"
    "  SET v_stem = CONCAT(LEFT(p_column, 20), '_', SUBSTR(MD5(p_ns), 1, 8));"

    // Canonical extJSON hides every non-string scalar inside a type wrapper, so
    // reading a field means trying each encoding in turn. This mirrors the
    // translator's `scalar_expr`; the two must list the same wrappers. A stored
    // function would say it once, but MariaDB forbids those inside generated
    // columns, so the COALESCE is spelled out here.
    "  SET v_read = CONCAT("
    "   'COALESCE(JSON_VALUE(~DOC~,', QUOTE(v_path), ')',"
    "   ',JSON_VALUE(~DOC~,', QUOTE(CONCAT(v_path, '.\"$numberInt\"')), ')',"
    "   ',JSON_VALUE(~DOC~,', QUOTE(CONCAT(v_path, '.\"$numberLong\"')), ')',"
    "   ',JSON_VALUE(~DOC~,', QUOTE(CONCAT(v_path, '.\"$numberDouble\"')), ')',"
    "   ',JSON_VALUE(~DOC~,', QUOTE(CONCAT(v_path, '.\"$date\".\"$numberLong\"')), ')',"
    "   ',JSON_VALUE(~DOC~,', QUOTE(CONCAT(v_path, '.\"$oid\"')), ')',"
    "   ')');"

    // Writing back means writing the wrapper the reader expects; a column of any
    // other type contributes a plain string, which is what it is.
    "  IF p_type REGEXP '^(TINY|SMALL|MEDIUM)?INT' THEN"
    "   SET v_patch ="
    "    CONCAT('CONCAT(''{\"', p_field, '\":{\"$numberInt\":\"'', ~COL~, ''\"}}'')');"
    "  ELSEIF p_type REGEXP '^BIGINT' THEN"
    "   SET v_patch ="
    "    CONCAT('CONCAT(''{\"', p_field, '\":{\"$numberLong\":\"'', ~COL~, ''\"}}'')');"
    "  ELSEIF p_type REGEXP '^(DOUBLE|FLOAT|DECIMAL|NUMERIC)' THEN"
    "   SET v_patch ="
    "    CONCAT('CONCAT(''{\"', p_field, '\":{\"$numberDouble\":\"'', ~COL~, ''\"}}'')');"
    "  ELSE"
    "   SET v_patch = CONCAT('JSON_OBJECT(', QUOTE(p_field), ', ~COL~)');"
    "  END IF;"

    "  INSERT INTO chimera_meta.projections (db_name, coll_name, column_name, field, mode)"
    "  VALUES (v_db, v_coll, p_column, p_field, p_mode)"
    "  ON DUPLICATE KEY UPDATE field = p_field, mode = p_mode;"

    "  IF p_mode = 'forward' THEN"
    "   SET @chimera_ddl = CONCAT('ALTER TABLE ', v_tbl, ' ADD COLUMN ', v_col, ' ', p_type,"
    "    ' AS (', REPLACE(v_read, '~DOC~', 'doc'), ') VIRTUAL');"
    "   PREPARE chimera_stmt FROM @chimera_ddl;"
    "   EXECUTE chimera_stmt;"
    "   DEALLOCATE PREPARE chimera_stmt;"
    "  ELSE"
    "   SET @chimera_ddl ="
    "    CONCAT('ALTER TABLE ', v_tbl, ' ADD COLUMN ', v_col, ' ', p_type, ' NULL');"
    "   PREPARE chimera_stmt FROM @chimera_ddl;"
    "   EXECUTE chimera_stmt;"
    "   DEALLOCATE PREPARE chimera_stmt;"

    // A BEFORE trigger that assigns NEW.doc needs UPDATE on those columns, and
    // the triggers belong to the locked owner account. The grant is column- and
    // table-scoped so bidirectionality never widens write access beyond the one
    // table that asked for it.
    "   SET @chimera_ddl = CONCAT('GRANT UPDATE (doc, ', p_column, ') ON ', v_tbl,"
    "    ' TO ''chimera''@''localhost''');"
    "   PREPARE chimera_stmt FROM @chimera_ddl;"
    "   EXECUTE chimera_stmt;"
    "   DEALLOCATE PREPARE chimera_stmt;"

    // Backfill, so the column is already true of existing rows before any
    // trigger has a chance to fire.
    "   SET @chimera_ddl = CONCAT('UPDATE ', v_tbl, ' SET ', v_col, ' = ',"
    "    REPLACE(v_read, '~DOC~', 'doc'));"
    "   PREPARE chimera_stmt FROM @chimera_ddl;"
    "   EXECUTE chimera_stmt;"
    "   DEALLOCATE PREPARE chimera_stmt;"

    // On INSERT the column wins when it was given a value, because supplying it
    // is the only way a SQL client can express the field at all. The old value is
    // removed before the patch is merged: merge-patch descends into objects, and
    // every scalar here lives inside a type wrapper, so patching in place would
    // leave a document carrying two different wrappers for one field.
    "   SET @chimera_ddl = CONCAT("
    "    'CREATE OR REPLACE DEFINER=''chimera''@''localhost'' TRIGGER `', v_db,"
    "    '`.`proj_bi_', v_stem, '` BEFORE INSERT ON ', v_tbl, ' FOR EACH ROW BEGIN',"
    "    ' IF NEW.', v_col, ' IS NOT NULL THEN SET NEW.doc = JSON_MERGE_PATCH(',"
    "    'JSON_REMOVE(NEW.doc, ', QUOTE(v_path), '), ',"
    "    REPLACE(v_patch, '~COL~', CONCAT('NEW.', v_col)), ');',"
    "    ' ELSE SET NEW.', v_col, ' = ', REPLACE(v_read, '~DOC~', 'NEW.doc'), ';',"
    "    ' END IF; END');"
    "   PREPARE chimera_stmt FROM @chimera_ddl;"
    "   EXECUTE chimera_stmt;"
    "   DEALLOCATE PREPARE chimera_stmt;"

    // On UPDATE, whichever side actually changed is the side that wins: a wire
    // write touches only `doc`, a SQL write touches only the column.
    "   SET @chimera_ddl = CONCAT("
    "    'CREATE OR REPLACE DEFINER=''chimera''@''localhost'' TRIGGER `', v_db,"
    "    '`.`proj_bu_', v_stem, '` BEFORE UPDATE ON ', v_tbl, ' FOR EACH ROW BEGIN',"
    "    ' IF NOT (NEW.', v_col, ' <=> OLD.', v_col, ') THEN',"
    "    ' SET NEW.doc = JSON_MERGE_PATCH(JSON_REMOVE(NEW.doc, ', QUOTE(v_path), '), ',"
    "    REPLACE(v_patch, '~COL~', CONCAT('NEW.', v_col)), ');',"
    "    ' ELSE SET NEW.', v_col, ' = ', REPLACE(v_read, '~DOC~', 'NEW.doc'), ';',"
    "    ' END IF; END');"
    "   PREPARE chimera_stmt FROM @chimera_ddl;"
    "   EXECUTE chimera_stmt;"
    "   DEALLOCATE PREPARE chimera_stmt;"
    "  END IF;"
    " END";

}  // namespace

void install_projection_support(SqlSession& sql) {
  sql.exec("CREATE DATABASE IF NOT EXISTS chimera_meta");
  sql.exec(kProjectionsTable);
  sql.exec(kAddProjection);
}

}  // namespace chimera
