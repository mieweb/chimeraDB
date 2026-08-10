// Error parity (M4.4). Codes, not prose: a driver branches on the code, and the
// wording is MongoDB's to change between releases.

show("find on unknown collection", run({ find: "never_created" }));
print("  documents " + tojson(fetch("never_created")));

show("count on unknown collection", run({ count: "never_created" }));
show("drop unknown collection", run({ drop: "never_created" }));
show("listIndexes on unknown collection", run({ listIndexes: "never_created" }));

run({ insert: "docs", documents: [{ _id: 1, n: 1 }] });

show("duplicate key", run({ insert: "docs", documents: [{ _id: 1, n: 2 }] }));
var dup = run({ insert: "docs", documents: [{ _id: 1, n: 2 }] });
print("  writeErrors " + dup.writeErrors.map(function (e) {
  return "index=" + e.index + " code=" + e.code;
}).join(", "));

show("unknown filter operator", run({ find: "docs", filter: { n: { $nosuchop: 1 } } }));
show("unknown update operator", run({
  update: "docs",
  updates: [{ q: { _id: 1 }, u: { $nosuchop: { n: 1 } } }]
}));
var bad = run({ update: "docs", updates: [{ q: { _id: 1 }, u: { $nosuchop: { n: 1 } } }] });
print("  writeErrors " + (bad.writeErrors || []).map(function (e) {
  return "index=" + e.index + " code=" + e.code;
}).join(", "));

show("unknown command", run({ definitelyNotACommand: 1 }));
show("missing distinct key", run({ distinct: "docs" }));
show("createIndexes without key", run({ createIndexes: "docs", indexes: [{ name: "x" }] }));
