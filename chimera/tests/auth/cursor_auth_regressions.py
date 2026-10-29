"""Authorization regressions shared by plaintext and TLS acceptance runs."""

import json


def _literal(value):
    return "CONVERT(X'" + value.encode().hex() + "' USING utf8mb4)"


def run(writer, reader, sql, rejected):
    db = writer.auth_test_allowed
    read_db = reader.auth_test_allowed
    reserved = '$cmd.auth_cursor_regression'
    cursors = []
    try:
        db.command({'drop': reserved})
        db.command({'insert': reserved, 'documents': [{'_id': 1}, {'_id': 2}]})
        buffered = read_db.command({'find': reserved, 'batchSize': 1})['cursor']['id']
        watching = read_db.command({'aggregate': reserved,
                                    'pipeline': [{'$changeStream': {}}], 'cursor': {}})['cursor']['id']
        assert buffered and watching
        cursors = [buffered, watching]
        sql("REVOKE SELECT ON auth_test_allowed.* FROM 'auth_test_reader'@'localhost'")
        try:
            db.command({'insert': reserved, 'documents': [{'_id': 'after-revocation'}]})
            for label, cursor in [('buffered cursor', buffered), ('change stream', watching)]:
                rejected('$cmd collection ' + label + ' rechecks revoked SELECT',
                         lambda c=cursor: read_db.command({'getMore': c, 'collection': reserved,
                                                         'maxTimeMS': 1}))
        finally:
            sql("GRANT SELECT ON auth_test_allowed.* TO 'auth_test_reader'@'localhost'")
    finally:
        if cursors:
            read_db.command({'killCursors': reserved, 'cursors': cursors})
        db.command({'drop': reserved})
    print('  ok  real $cmd.* collection cursors recheck revoked grants')

    # An existing oplog can have inherited any schema collation. Force a
    # case-insensitive one before opening the watch, so this also covers servers
    # upgrading with their existing oplog rather than newly created DDL.
    charset, collation = sql(
        "SELECT CHARACTER_SET_NAME,COLLATION_NAME FROM information_schema.COLUMNS "
        "WHERE TABLE_SCHEMA='chimera_meta' AND TABLE_NAME='oplog' AND COLUMN_NAME='ns'").split('\t')
    assert all(part.replace('_', '').isalnum() for part in (charset, collation))
    collection = 'auth_case_watch_regression'
    secret_db = 'AUTH_TEST_ALLOWED'
    secret_table = '`' + secret_db + '`.`' + collection + '`'
    case_sensitive_names = sql('SELECT @@lower_case_table_names') == '0'
    created_secret_db = False
    secret_table_created = False
    watch_id = None
    try:
        sql('ALTER TABLE chimera_meta.oplog MODIFY ns VARCHAR(512) '
            'CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci NOT NULL')
        db.command({'drop': collection})
        db.command({'create': collection})
        if case_sensitive_names:
            created_secret_db = sql(
                "SELECT COUNT(*) FROM information_schema.SCHEMATA "
                "WHERE CAST(SCHEMA_NAME AS BINARY)=CAST('AUTH_TEST_ALLOWED' AS BINARY)") == '0'
            sql('CREATE DATABASE IF NOT EXISTS `' + secret_db + '`')
            sql('CREATE TABLE ' + secret_table + ' (_id VARBINARY(255) PRIMARY KEY, doc JSON NOT NULL)')
            secret_table_created = True
            sql("CALL chimera_meta.chimera_adopt_table('" + secret_db + "','" + collection + "')")
            rejected('case-distinct database keeps separate grants',
                     lambda: reader[secret_db].command({'find': collection}))
        watch_id = read_db.command({'aggregate': collection,
                                    'pipeline': [{'$changeStream': {}}], 'cursor': {}})['cursor']['id']
        secret = json.dumps({'_id': 'secret-case-document', 'value': 'private'})
        if case_sensitive_names:
            sql('INSERT INTO ' + secret_table + ' (_id,doc) VALUES (X\'01\',' + _literal(secret) + ')')
        else:
            # Case-folding filesystems cannot host both schemas. Exercise the
            # same retained-oplog boundary with a distinct namespace directly.
            sql("CALL chimera_meta.oplog_append('i'," +
                _literal(secret_db + '.' + collection) + ',' + _literal(secret) + ',NULL)')
        db.command({'insert': collection, 'documents': [{'_id': 'public-case-document'}]})
        events = read_db.command({'getMore': watch_id, 'collection': collection,
                                  'maxTimeMS': 1})['cursor']['nextBatch']
        assert len(events) == 1, events
        assert events[0]['ns'] == {'db': 'auth_test_allowed', 'coll': collection}, events
        assert events[0]['fullDocument']['_id'] == 'public-case-document', events
    finally:
        try:
            sql('ALTER TABLE chimera_meta.oplog MODIFY ns VARCHAR(512) CHARACTER SET ' +
                charset + ' COLLATE ' + collation + ' NOT NULL')
        finally:
            if watch_id:
                read_db.command({'killCursors': collection, 'cursors': [watch_id]})
            db.command({'drop': collection})
            if secret_table_created:
                sql('DROP TABLE ' + secret_table)
                sql('DELETE FROM chimera_meta.collections WHERE CAST(db_name AS BINARY)=' +
                    'CAST(' + _literal(secret_db) + ' AS BINARY) AND coll_name=' + _literal(collection))
            if created_secret_db:
                sql('DROP DATABASE `' + secret_db + '`')
    print('  ok  change streams use byte-exact namespaces on an existing CI oplog')
