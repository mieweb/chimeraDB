"""Exercise installed artifacts through real Mongo and SQL client libraries."""
import json
import os
import sys
import time

import pymongo
import pymysql

host = os.environ.get("CHIMERA_HOST", "db")
mongo = pymongo.MongoClient(host, 27017, directConnection=True, serverSelectionTimeoutMS=5000)
sql = pymysql.connect(host=host, user="root", password=os.environ["MARIADB_ROOT_PASSWORD"],
                      autocommit=True, connect_timeout=5)
database = "chimera_release_smoke"
collection = mongo[database].items


def scalar(statement, parameters=None):
    with sql.cursor() as cursor:
        cursor.execute(statement, parameters)
        return cursor.fetchone()[0]


try:
    # Readiness must include a first ping on an empty installation, before any
    # insert has initialized the operationTime clock as a side effect.
    assert mongo.admin.command("ping")["ok"] == 1
    assert scalar("SELECT plugin_status FROM information_schema.plugins "
                  "WHERE plugin_name='chimera_mongo'") == "ACTIVE"
    if "--verify-persistence" in sys.argv:
        assert collection.find_one({"_id": "persisted"})["value"] == 42
        assert json.loads(scalar(f"SELECT doc FROM {database}.items WHERE _id=%s",
                                 (b"\x02persisted",)))["_id"] == "persisted"
        print("PASS: Mongo and SQL data survived container replacement")
    else:
        collection.insert_one({"_id": "persisted", "value": 42})
        assert json.loads(scalar(f"SELECT doc FROM {database}.items WHERE _id=%s",
                                 (b"\x02persisted",)))["_id"] == "persisted"
        collection.insert_one({"_id": "temporary", "value": 1})
        assert collection.update_one({"_id": "temporary"}, {"$inc": {"value": 2}}).modified_count == 1
        assert collection.find_one({"_id": "temporary"})["value"] == 3
        assert collection.delete_one({"_id": "temporary"}).deleted_count == 1
        assert collection.find_one({"_id": "temporary"}) is None
        # Opening the stream before the SQL write verifies trigger-backed reactivity.
        with collection.watch(max_await_time_ms=500) as stream:
            with sql.cursor() as cursor:
                cursor.execute(f"INSERT INTO {database}.items (_id,doc) VALUES (%s,%s)",
                               (b"\x02sql-event", json.dumps({"_id": "sql-event", "value": 17})))
            deadline = time.monotonic() + 15
            event = None
            while time.monotonic() < deadline and event is None:
                event = stream.try_next()
            assert event and event["operationType"] == "insert", event
            assert event["fullDocument"]["_id"] == "sql-event", event
        assert collection.find_one({"_id": "sql-event"})["value"] == 17
        assert scalar("SELECT COUNT(*) FROM chimera_meta.oplog WHERE ns=%s", (database + ".items",)) >= 2
        result = scalar("SELECT mongo(%s,%s)", (database, "db.items.findOne({_id: 'persisted'})"))
        assert json.loads(result)["_id"] == "persisted"
        # A non-loopback Mongo listener must keep the SQL gateway closed.
        try:
            mongo[database].command({"chimeraSql": "SELECT 1"})
            raise AssertionError("SQL gateway unexpectedly enabled on the container Mongo listener")
        except pymongo.errors.OperationFailure as error:
            assert error.code == 13, error
        print("PASS: wire CRUD, SQL visibility, SQL→change stream, oplog, mongo() and gateway isolation")
finally:
    sql.close()
    mongo.close()
