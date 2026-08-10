// Prepended to every differential spec so both endpoints start from the same
// blank state and report results the same way.

db = db.getSiblingDB("difftest");
db.dropDatabase();

// Commands, not shell helpers: the helpers differ between shell builds, and we
// are comparing servers, not shells.
function run(command) {
  return db.runCommand(command);
}

// Errors are compared by code, not by prose. Message wording is MongoDB's to
// change; the code is the contract a driver actually branches on.
function show(label, result) {
  if (result.ok === 1 || result.ok === true) {
    print(label + ": ok");
  } else {
    print(label + ": error code=" + result.code + " codeName=" + result.codeName);
  }
}

function showDocs(label, docs) {
  print(label + ": " + docs.length + " document(s)");
  docs.forEach(function (doc) {
    print("  " + tojson(doc, "", true));
  });
}

// find() through the command, sorted by _id unless the spec says otherwise, so
// that storage-order differences never masquerade as behaviour differences.
function fetch(collection, options) {
  options = options || {};
  var command = { find: collection };
  Object.keys(options).forEach(function (key) {
    command[key] = options[key];
  });
  if (command.sort === undefined) {
    command.sort = { _id: 1 };
  }
  var result = run(command);
  if (result.ok !== 1) {
    return null;
  }
  return result.cursor.firstBatch;
}
