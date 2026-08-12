// Change streams: opening one, the shape of the events it delivers, resuming
// from a token, and what happens when a stream is abandoned or asked to resume
// from nonsense.
//
// Resume tokens are deliberately never printed. MongoDB's are opaque structures
// encoding a cluster time, a collection UUID and type bits; ours are a rendering
// of an oplog sequence. Neither is the other's business — what matters is that a
// token handed back resumes in the right place, and that is asserted through
// behaviour.
//
// One divergence shapes this whole file. Asked to resume from a token whose
// following events are already in the oplog, the reference build used here
// resumes at the head of the stream and never replays the gap; chimera replays
// it. Replay is what a reconnecting client depends on, so it is asserted
// directly against chimera in scripts/demo-changestream.sh, where it can be
// checked rather than merely compared. Everything below is written so both
// servers answer identically: streams are opened before the writes they are
// expected to observe.

// Both servers deliver only durable events, and an awaitData getMore parks until
// it has something or the deadline passes. Loop rather than assuming one round
// trip is enough, so a majority-commit lag cannot masquerade as a lost event.
function drain(cursorId, collection, want) {
  var events = [];
  for (var attempt = 0; attempt < 10 && events.length < want; attempt++) {
    var batch = run({ getMore: cursorId, collection: collection, maxTimeMS: 1000 });
    if (batch.ok !== 1) {
      print("  drain stopped: code=" + batch.code + " codeName=" + batch.codeName);
      return events;
    }
    events = events.concat(batch.cursor.nextBatch);
  }
  return events;
}

// Everything volatile lives in the fields left out here: the event token, the
// cluster time and the wall clock.
function showEvent(event) {
  print("  " + event.operationType +
        " ns=" + event.ns.db + "." + event.ns.coll +
        " key=" + tojson(event.documentKey, "", true) +
        " doc=" + (event.fullDocument === undefined ? "-" : tojson(event.fullDocument, "", true)) +
        " token=" + (event._id !== undefined && event._id._data !== undefined));
}

run({ insert: "events", documents: [{ _id: 1, n: 1 }] });

// Opening never replays history: a change stream is a position, not a query.
var opened = run({ aggregate: "events", pipeline: [{ $changeStream: {} }], cursor: {} });
print("open firstBatch " + opened.cursor.firstBatch.length +
      " live=" + (opened.cursor.id != 0) +
      " ns=" + opened.cursor.ns +
      " resumable=" + (opened.cursor.postBatchResumeToken !== undefined));

// A whole-document `u` is a replace on both servers. A `$set` is an `update`
// carrying an `updateDescription` on MongoDB and a `replace` here, because our
// oplog records post-images rather than modifiers — so the shapes compared here
// avoid `$set` rather than smuggle that difference in.
run({ insert: "events", documents: [{ _id: 2, n: 2 }] });
run({ update: "events", updates: [{ q: { _id: 2 }, u: { _id: 2, n: 20 } }] });
run({ delete: "events", deletes: [{ q: { _id: 2 }, limit: 1 }] });

var events = drain(opened.cursor.id, "events", 3);
print("events " + events.length);
events.forEach(showEvent);

// A token is accepted, the stream carries on from it, and the event the token
// names is never delivered a second time.
if (events.length === 3) {
  var resumed = run({
    aggregate: "events",
    pipeline: [{ $changeStream: { startAfter: events[0]._id } }],
    cursor: {}
  });
  print("resume accepted " + (resumed.ok === 1 && resumed.cursor.id != 0));

  run({ insert: "events", documents: [{ _id: 3, n: 3 }] });
  var after = drain(resumed.cursor.id, "events", 1);
  var repeated = after.some(function (event) {
    return event.operationType === "insert" && event.documentKey._id === 2;
  });
  var sawNew = after.some(function (event) {
    return event.documentKey._id === 3;
  });
  print("resumed sees later writes " + sawNew + " repeats its own token " + repeated);
}

// A stream with nothing to say still reports where it is, so a client that has
// been idle can come back without replaying from the beginning.
var quiet = run({ aggregate: "events", pipeline: [{ $changeStream: {} }], cursor: {} });
var idle = run({ getMore: quiet.cursor.id, collection: "events", maxTimeMS: 500 });
print("quiet batch " + idle.cursor.nextBatch.length +
      " live=" + (idle.cursor.id != 0) +
      " resumable=" + (idle.cursor.postBatchResumeToken !== undefined));

// Abandoning a stream kills it like any other cursor.
var killed = run({ killCursors: "events", cursors: [quiet.cursor.id] });
print("killed " + killed.cursorsKilled.length + " notFound " + killed.cursorsNotFound.length);
show("getMore after kill", run({ getMore: quiet.cursor.id, collection: "events" }));

// A token neither server issued is refused. The code differs — MongoDB fails
// parsing its own token format, we fail ours — so only the refusal is asserted.
var bogus = run({
  aggregate: "events",
  pipeline: [{ $changeStream: { resumeAfter: { _data: "not-a-token" } } }],
  cursor: {}
});
print("bogus token refused " + (bogus.ok !== 1));

// Documented divergence: MongoDB runs stages after `$changeStream`, we refuse
// them with NotImplemented rather than accept a filter and silently not apply
// it. Both are defensible; asserting either would make this spec a record of our
// own opinion instead of a comparison, so it asserts only that the server
// answered with one of the two documented outcomes.
var extra = run({
  aggregate: "events",
  pipeline: [{ $changeStream: {} }, { $match: { operationType: "insert" } }],
  cursor: {}
});
print("stage after $changeStream answered " + (extra.ok === 1 || extra.code === 238));
