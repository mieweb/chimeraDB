// Sorting, projection and paging — all of which ChimeraDB applies above SQL
// because BSON's cross-type ordering has no SQL equivalent.

run({
  insert: "items",
  documents: [
    { _id: 1, name: "delta", rank: 2, group: "x", detail: { colour: "red" } },
    { _id: 2, name: "alpha", rank: 1, group: "y", detail: { colour: "blue" } },
    { _id: 3, name: "charlie", rank: 2, group: "x", detail: { colour: "green" } },
    { _id: 4, name: "bravo", rank: 10, group: "y" },
    { _id: 5, name: "echo", group: "x" }
  ]
});

showDocs("by rank asc", fetch("items", { sort: { rank: 1, _id: 1 } }));
showDocs("by rank desc", fetch("items", { sort: { rank: -1, _id: 1 } }));
showDocs("by name", fetch("items", { sort: { name: 1 } }));
showDocs("by nested", fetch("items", { sort: { "detail.colour": 1, _id: 1 } }));

showDocs("include", fetch("items", { projection: { name: 1 } }));
showDocs("include without _id", fetch("items", { projection: { name: 1, _id: 0 } }));
showDocs("exclude", fetch("items", { projection: { detail: 0, group: 0 } }));
showDocs("nested include", fetch("items", { projection: { "detail.colour": 1 } }));

showDocs("skip", fetch("items", { skip: 2 }));
showDocs("limit", fetch("items", { limit: 2 }));
showDocs("skip and limit", fetch("items", { skip: 1, limit: 2 }));
showDocs("sort skip limit", fetch("items", { sort: { name: 1 }, skip: 1, limit: 2 }));
