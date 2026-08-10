// delete, findAndModify, count and distinct.

function seed() {
  run({ delete: "tasks", deletes: [{ q: {}, limit: 0 }] });
  run({
    insert: "tasks",
    documents: [
      { _id: 1, title: "write", done: false, owner: "ada", labels: ["a", "b"] },
      { _id: 2, title: "review", done: true, owner: "bob", labels: ["b"] },
      { _id: 3, title: "ship", done: false, owner: "ada", labels: ["c", "a"] },
      { _id: 4, title: "rest", done: true, owner: "cy" }
    ]
  });
}

seed();
var one = run({ delete: "tasks", deletes: [{ q: { done: true }, limit: 1 }] });
print("delete one n=" + one.n);
var many = run({ delete: "tasks", deletes: [{ q: { done: false }, limit: 0 }] });
print("delete many n=" + many.n);
showDocs("after deletes", fetch("tasks"));

seed();
print("count all " + run({ count: "tasks" }).n);
print("count filtered " + run({ count: "tasks", query: { owner: "ada" } }).n);
print("count with limit " + run({ count: "tasks", limit: 2 }).n);
print("count with skip " + run({ count: "tasks", skip: 3 }).n);

print("distinct owner " + tojson(run({ distinct: "tasks", key: "owner" }).values.sort()));
print("distinct labels " + tojson(run({ distinct: "tasks", key: "labels" }).values.sort()));
print("distinct filtered " +
      tojson(run({ distinct: "tasks", key: "title", query: { done: true } }).values.sort()));

seed();
var modified = run({
  findAndModify: "tasks",
  query: { done: false },
  sort: { _id: -1 },
  update: { $set: { done: true } }
});
print("findAndModify old " + tojson(modified.value, "", true));
print("  lastErrorObject " + tojson(modified.lastErrorObject, "", true));

var returned = run({
  findAndModify: "tasks",
  query: { owner: "ada" },
  sort: { _id: 1 },
  update: { $set: { title: "rewritten" } },
  new: true
});
print("findAndModify new " + tojson(returned.value, "", true));

var removed = run({ findAndModify: "tasks", query: { _id: 4 }, remove: true });
print("findAndModify removed " + tojson(removed.value, "", true));

var missing = run({ findAndModify: "tasks", query: { _id: 404 }, update: { $set: { x: 1 } } });
print("findAndModify missing " + tojson(missing.value, "", true) +
      " " + tojson(missing.lastErrorObject, "", true));

var created = run({
  findAndModify: "tasks",
  query: { _id: 500 },
  update: { $set: { title: "fresh" } },
  upsert: true,
  new: true
});
print("findAndModify upsert " + tojson(created.value, "", true));

showDocs("final", fetch("tasks"));
