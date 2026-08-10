// The third and last translation unit that sees MariaDB's headers — and the
// only one that sees its *UDF* headers. It holds no logic: it converts a SQL
// call into a pair of std::strings, hands them to the gateway, and turns an
// exception back into a SQL error. Everything about what a statement means
// lives in mongogateway.cc, which has no idea a UDF exists.
#include <my_global.h>

// sql_class.h only defines THD for code that identifies itself as part of the
// server, which an in-tree plugin is. It is defined here and nowhere else: this
// file's whole job is to touch the server, so the exposure stops at its edge.
#define MYSQL_SERVER 1

#include <mysql.h>
#include <mysql/plugin.h>
#include <my_sys.h>
#include <mysqld_error.h>
#include <sql_class.h>  // current_thd, for the database the caller is already in

#include <new>
#include <string>

#include "mongogateway.h"

namespace {

// The result of one call. A UDF's own 255-byte buffer is far too small for a
// document, so the string lives here and stays alive until the next row.
struct MongoUdfState {
  std::string result;
};

std::string argument_text(UDF_ARGS* args, unsigned index) {
  if (args->args[index] == nullptr) return {};
  return std::string(args->args[index], args->lengths[index]);
}

// `mongo('<statement>')` uses the database the session is already in, which is
// what a SQL user means by "here". `mongo('<database>', '<statement>')` says so
// explicitly, for a caller with no current database or one crossing schemas.
bool split_arguments(UDF_ARGS* args, std::string* db, std::string* statement) {
  if (args->arg_count == 2) {
    *db = argument_text(args, 0);
    *statement = argument_text(args, 1);
    return true;
  }
  *statement = argument_text(args, 0);
  THD* thd = current_thd;
  if (thd != nullptr && thd->db.str != nullptr) *db = std::string(thd->db.str, thd->db.length);
  return true;
}

}  // namespace

extern "C" {

my_bool mongo_init(UDF_INIT* initid, UDF_ARGS* args, char* message) {
  if (args->arg_count < 1 || args->arg_count > 2) {
    strcpy(message, "mongo() takes mongo('<statement>') or mongo('<database>', '<statement>')");
    return 1;
  }
  for (unsigned i = 0; i < args->arg_count; ++i) args->arg_type[i] = STRING_RESULT;

  auto* state = new (std::nothrow) MongoUdfState();
  if (state == nullptr) {
    strcpy(message, "mongo(): out of memory");
    return 1;
  }
  initid->ptr = reinterpret_cast<char*>(state);
  initid->maybe_null = 1;
  initid->const_item = 0;
  // A collection scan can return a document far larger than the default; the
  // cap is the wire protocol's own document limit.
  initid->max_length = 16 * 1024 * 1024;
  return 0;
}

void mongo_deinit(UDF_INIT* initid) {
  delete reinterpret_cast<MongoUdfState*>(initid->ptr);
  initid->ptr = nullptr;
}

char* mongo(UDF_INIT* initid, UDF_ARGS* args, char* result, unsigned long* length, char* is_null,
            char* error) {
  auto* state = reinterpret_cast<MongoUdfState*>(initid->ptr);
  (void)result;

  std::string db;
  std::string statement;
  split_arguments(args, &db, &statement);
  if (statement.empty()) {
    *is_null = 1;
    return nullptr;
  }

  try {
    state->result = chimera::run_mongo_gateway(db, statement);
  } catch (const std::exception& e) {
    // A failed write must stop the statement, not return a document that says
    // it failed — a SQL caller would go on to store it.
    my_printf_error(ER_UNKNOWN_ERROR, "mongo(): %s", MYF(0), e.what());
    *error = 1;
    *is_null = 1;
    return nullptr;
  }

  *length = static_cast<unsigned long>(state->result.size());
  return const_cast<char*>(state->result.c_str());
}

}  // extern "C"
