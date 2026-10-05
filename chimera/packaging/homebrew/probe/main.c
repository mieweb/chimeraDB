/* Exercise the installed Mongo listener with the public C driver API. */
#include <mongoc/mongoc.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
  if (argc < 2 || argc > 3 || (argc == 3 && strcmp(argv[2], "--ping-only"))) {
    fprintf(stderr, "usage: chimeradb-wire-smoke URI [--ping-only]\n");
    return 2;
  }
  int status = 1;
  bson_error_t error;
  mongoc_init();
  mongoc_client_t *client = mongoc_client_new(argv[1]);
  if (!client) {
    fprintf(stderr, "invalid Mongo URI\n");
    mongoc_cleanup();
    return 1;
  }
  mongoc_client_set_error_api(client, 2);
  bson_t *ping = BCON_NEW("ping", BCON_INT32(1));
  if (!mongoc_client_command_simple(client, "admin", ping, NULL, NULL, &error)) {
    fprintf(stderr, "Mongo ping failed: %s\n", error.message);
    bson_destroy(ping);
    goto done;
  }
  bson_destroy(ping);
  if (argc == 2) {
    mongoc_collection_t *collection = mongoc_client_get_collection(client, "package_smoke", "wire_docs");
    bson_t *document = BCON_NEW("_id", BCON_UTF8("wire-persisted"), "value", BCON_UTF8("brew-wire"));
    const bool inserted = mongoc_collection_insert_one(collection, document, NULL, NULL, &error);
    bson_destroy(document);
    mongoc_collection_destroy(collection);
    if (!inserted) {
      fprintf(stderr, "Mongo insert failed: %s\n", error.message);
      goto done;
    }
  }
  status = 0;
done:
  mongoc_client_destroy(client);
  mongoc_cleanup();
  return status;
}
