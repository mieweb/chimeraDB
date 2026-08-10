// Batching: a find that does not fit one batch leaves a cursor behind, and the
// driver drains it with getMore or abandons it with killCursors.

var documents = [];
for (var i = 0; i < 12; i++) {
  documents.push({ _id: i, n: i });
}
run({ insert: "numbers", documents: documents });

var first = run({ find: "numbers", sort: { _id: 1 }, batchSize: 5 });
print("first batch " + first.cursor.firstBatch.length + " open=" + (first.cursor.id != 0));

var second = run({ getMore: first.cursor.id, collection: "numbers", batchSize: 5 });
print("second batch " + second.cursor.nextBatch.length + " open=" + (second.cursor.id != 0));

var third = run({ getMore: first.cursor.id, collection: "numbers", batchSize: 5 });
print("third batch " + third.cursor.nextBatch.length + " open=" + (third.cursor.id != 0));

// The cursor is exhausted, so the id is gone.
show("getMore after exhaustion",
     run({ getMore: first.cursor.id, collection: "numbers", batchSize: 5 }));

var abandoned = run({ find: "numbers", sort: { _id: 1 }, batchSize: 2 });
var killed = run({ killCursors: "numbers", cursors: [abandoned.cursor.id] });
print("killed " + killed.cursorsKilled.length + " notFound " + killed.cursorsNotFound.length);
show("getMore after kill",
     run({ getMore: abandoned.cursor.id, collection: "numbers", batchSize: 2 }));

// singleBatch closes the cursor even when documents remain.
var single = run({ find: "numbers", sort: { _id: 1 }, batchSize: 3, singleBatch: true });
print("singleBatch " + single.cursor.firstBatch.length + " open=" + (single.cursor.id != 0));
