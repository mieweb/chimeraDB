// Update operators, multi, and upsert.

run({
  insert: "accounts",
  documents: [
    { _id: 1, owner: "ada", balance: 100, flags: ["new"] },
    { _id: 2, owner: "bob", balance: 50, flags: ["new", "vip"] },
    { _id: 3, owner: "cy", balance: 50 }
  ]
});

show("set", run({
  update: "accounts",
  updates: [{ q: { _id: 1 }, u: { $set: { balance: 120, note: "topped up" } } }]
}));
showDocs("after set", fetch("accounts", { filter: { _id: 1 } }));

show("inc", run({
  update: "accounts",
  updates: [{ q: { _id: 2 }, u: { $inc: { balance: 25 } } }]
}));

show("unset", run({
  update: "accounts",
  updates: [{ q: { _id: 1 }, u: { $unset: { note: "" } } }]
}));

show("push and addToSet", run({
  update: "accounts",
  updates: [
    { q: { _id: 1 }, u: { $push: { flags: "audited" } } },
    { q: { _id: 2 }, u: { $addToSet: { flags: "vip" } } }
  ]
}));

// nModified must distinguish "matched but unchanged" from "actually rewritten".
var idempotent = run({
  update: "accounts",
  updates: [{ q: { _id: 3 }, u: { $set: { balance: 50 } } }]
});
print("idempotent n=" + idempotent.n + " nModified=" + idempotent.nModified);

var multi = run({
  update: "accounts",
  updates: [{ q: { balance: { $lte: 100 } }, u: { $set: { tier: "basic" } }, multi: true }]
});
print("multi n=" + multi.n + " nModified=" + multi.nModified);

var single = run({
  update: "accounts",
  updates: [{ q: { balance: { $lte: 1000 } }, u: { $set: { touched: true } } }]
});
print("single n=" + single.n + " nModified=" + single.nModified);

var upsert = run({
  update: "accounts",
  updates: [{ q: { _id: 99 }, u: { $set: { owner: "new" } }, upsert: true }]
});
print("upsert n=" + upsert.n + " nModified=" + upsert.nModified +
      " upserted=" + tojson(upsert.upserted, "", true));

show("replacement document", run({
  update: "accounts",
  updates: [{ q: { _id: 3 }, u: { owner: "cy", balance: 0 } }]
}));

showDocs("final", fetch("accounts"));
