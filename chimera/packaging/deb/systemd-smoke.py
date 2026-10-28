"""Native Debian acceptance through root Unix-socket SQL and loopback Mongo."""
import argparse
import json
import time

import pymongo
import pymysql

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--socket", required=True)
parser.add_argument("--seed", action="store_true")
args = parser.parse_args()
database = "chimera_systemd_acceptance"
mongo = pymongo.MongoClient("127.0.0.1", 27017, directConnection=True,
                            serverSelectionTimeoutMS=5000, socketTimeoutMS=20000)
sql = pymysql.connect(unix_socket=args.socket, user="root", autocommit=True,
                      connect_timeout=5, read_timeout=20, write_timeout=20)


def scalar(statement, parameters=None):
    with sql.cursor() as cursor:
        cursor.execute(statement, parameters)
        return cursor.fetchone()[0]


try:
    # First Mongo operation, before writes can create the clock as a side effect.
    assert mongo.admin.command("ping")["ok"] == 1
    collection = mongo[database].items
    if args.seed:
        assert database not in mongo.list_database_names(), "acceptance database already exists"
        collection.insert_one({"_id": "persisted", "value": 42})
        collection.insert_one({"_id": "temporary", "value": 1})
        assert collection.update_one({"_id": "temporary"}, {"$inc": {"value": 2}}).modified_count == 1
        assert collection.find_one({"_id": "temporary"})["value"] == 3
        assert collection.delete_one({"_id": "temporary"}).deleted_count == 1
        with collection.watch(max_await_time_ms=500) as stream:
            with sql.cursor() as cursor:
                cursor.execute(f"INSERT INTO {database}.items (_id,doc) VALUES (%s,%s)",
                               (b"\x02sql-event", json.dumps({"_id": "sql-event", "value": 17})))
            deadline = time.monotonic() + 15
            event = None
            while event is None and time.monotonic() < deadline:
                event = stream.try_next()
            assert event and event["operationType"] == "insert", event
            assert event["fullDocument"]["_id"] == "sql-event", event
    assert collection.find_one({"_id": "persisted"})["value"] == 42
    assert collection.find_one({"_id": "sql-event"})["value"] == 17
    assert collection.find_one({"_id": "temporary"}) is None
    document = scalar(f"SELECT doc FROM {database}.items WHERE _id=%s", (b"\x02persisted",))
    assert json.loads(document)["_id"] == "persisted"
    result = scalar("SELECT mongo(%s,%s)", (database, "db.items.findOne({_id: 'persisted'})"))
    assert json.loads(result)["_id"] == "persisted"
    # Loopback native installs intentionally permit the read-only SQL gateway.
    reply = mongo[database].command({"chimeraSql": "SELECT 1 AS n"})
    assert reply["cursor"]["firstBatch"][0]["n"] == 1
    assert scalar("SELECT COUNT(*) FROM chimera_meta.oplog WHERE ns=%s", (database + ".items",)) >= 2
    print("PASS: native driver ping, Mongo/SQL visibility, mongo() and loopback SQL gateway" +
          (", CRUD and SQL-triggered change stream" if args.seed else ", persistent documents"))
finally:
    sql.close()
    mongo.close()
