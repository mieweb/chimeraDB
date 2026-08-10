// Collection and index lifecycle.

show("create", run({ create: "widgets" }));
show("create again", run({ create: "widgets" }));

run({ insert: "widgets", documents: [{ _id: 1, sku: "aa", bin: 3 }] });
run({ create: "gadgets" });

function collectionNames() {
  return run({ listCollections: 1, nameOnly: true }).cursor.firstBatch
    .map(function (entry) { return entry.name; })
    .sort();
}
print("collections " + tojson(collectionNames()));

function indexSummary() {
  return run({ listIndexes: "widgets" }).cursor.firstBatch
    .map(function (entry) {
      return entry.name + "=" + tojson(entry.key) + (entry.unique ? " unique" : "");
    })
    .sort();
}
print("indexes " + tojson(indexSummary()));

var created = run({
  createIndexes: "widgets",
  indexes: [
    { key: { sku: 1 }, name: "sku_1", unique: true },
    { key: { bin: -1 }, name: "bin_-1" }
  ]
});
print("createIndexes before=" + created.numIndexesBefore + " after=" + created.numIndexesAfter);
print("indexes " + tojson(indexSummary()));

// Creating the same index twice is a no-op, not an error.
var again = run({ createIndexes: "widgets", indexes: [{ key: { sku: 1 }, name: "sku_1", unique: true }] });
print("repeat before=" + again.numIndexesBefore + " after=" + again.numIndexesAfter);

// The server does not derive a name from the key; the client must send one.
show("default index name", run({ createIndexes: "widgets", indexes: [{ key: { sku: 1, bin: 1 } }] }));
print("indexes " + tojson(indexSummary()));

show("dropIndexes by name", run({ dropIndexes: "widgets", index: "bin_-1" }));
print("indexes " + tojson(indexSummary()));

show("dropIndexes missing", run({ dropIndexes: "widgets", index: "nope_1" }));

show("drop", run({ drop: "gadgets" }));
print("collections " + tojson(collectionNames()));

show("drop missing", run({ drop: "gadgets" }));
