// The filter operators M2 compiles to SQL, exercised against real data. The M2
// unit tests only ever checked the generated SQL text, which is how a broken
// range comparison stayed green for a milestone.

run({
  insert: "readings",
  documents: [
    { _id: 1, sensor: "a", value: 10, ok: true, at: new Date("2024-01-01T00:00:00Z") },
    { _id: 2, sensor: "b", value: 20, ok: false, at: new Date("2024-06-01T00:00:00Z") },
    { _id: 3, sensor: "c", value: 30.5, ok: true, at: new Date("2025-01-01T00:00:00Z") },
    { _id: 4, sensor: "a", value: null, ok: true },
    { _id: 5, sensor: "d", nested: { depth: 7 }, tags: ["x", "y"] }
  ]
});

showDocs("eq", fetch("readings", { filter: { sensor: "a" } }));
showDocs("gt", fetch("readings", { filter: { value: { $gt: 15 } } }));
showDocs("gte", fetch("readings", { filter: { value: { $gte: 20 } } }));
showDocs("lt", fetch("readings", { filter: { value: { $lt: 30 } } }));
showDocs("ne", fetch("readings", { filter: { sensor: { $ne: "a" } } }));
showDocs("in", fetch("readings", { filter: { sensor: { $in: ["a", "c"] } } }));
showDocs("nin", fetch("readings", { filter: { sensor: { $nin: ["a", "c"] } } }));
showDocs("exists true", fetch("readings", { filter: { nested: { $exists: true } } }));
showDocs("exists false", fetch("readings", { filter: { value: { $exists: false } } }));
showDocs("bool", fetch("readings", { filter: { ok: false } }));
showDocs("dotted", fetch("readings", { filter: { "nested.depth": 7 } }));
showDocs("array member", fetch("readings", { filter: { tags: "x" } }));

showDocs("and", fetch("readings", {
  filter: { $and: [{ ok: true }, { value: { $gte: 10 } }] }
}));
showDocs("or", fetch("readings", {
  filter: { $or: [{ sensor: "d" }, { value: { $gt: 25 } }] }
}));
showDocs("not-in via nor", fetch("readings", {
  filter: { $nor: [{ sensor: "a" }, { sensor: "b" }] }
}));

// Dates have to compare as instants, not as their printed form.
showDocs("date gt", fetch("readings", {
  filter: { at: { $gt: new Date("2024-03-01T00:00:00Z") } }
}));
