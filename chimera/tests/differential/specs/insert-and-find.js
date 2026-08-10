// Inserting documents and reading them back, including the id the server has to
// generate when the client does not supply one.

show("insert three", run({
  insert: "people",
  documents: [
    { _id: 1, name: "ada", age: 36, tags: ["math", "engines"] },
    { _id: 2, name: "bob", age: 41, tags: [] },
    { _id: "grace", name: "grace", age: 45 }
  ]
}));

showDocs("all", fetch("people"));

show("insert without _id", run({ insert: "people", documents: [{ name: "anon" }] }));
print("count now " + run({ count: "people" }).n);

// The generated id must be an ObjectId — normalization hides its value, not its
// type, so a wrong type still fails the diff.
var generated = fetch("people", { filter: { name: "anon" } });
print("generated id type: " + (generated[0]._id instanceof ObjectId));

show("duplicate _id", run({ insert: "people", documents: [{ _id: 1, name: "clash" }] }));
print("count after clash " + run({ count: "people" }).n);

// An unordered batch keeps going past a failure; an ordered one stops.
show("unordered batch", run({
  insert: "people",
  ordered: false,
  documents: [{ _id: 1 }, { _id: 10 }, { _id: 2 }, { _id: 11 }]
}));
print("count after unordered " + run({ count: "people" }).n);
