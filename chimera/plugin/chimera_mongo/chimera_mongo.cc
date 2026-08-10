// The only file in ChimeraDB that includes MariaDB headers. It exists to
// declare the daemon plugin and hand the configured address and port to the
// listener; everything else about the Mongo head lives in server-agnostic code
// so it can be built and reasoned about without a server.
#include <my_global.h>
#include <mysql_version.h>
#include <mysql/plugin.h>
#include <sql_plugin.h>   // st_plugin_int, whose `data` field holds our listener

#include <memory>
#include <string>

#include "listener.h"
#include "oplog.h"
#include "sqlgateway.h"

static char *chimera_mongo_bind_value = nullptr;
static unsigned int chimera_mongo_port_value = 0;
static unsigned long long chimera_mongo_oplog_max_rows_value = 0;
static unsigned long long chimera_mongo_oplog_max_age_value = 0;
static char chimera_mongo_sql_writes_value = 0;
static std::unique_ptr<chimera::OplogPruner> chimera_mongo_pruner;

// Loopback by default and deliberately: there is no authentication yet, so a
// wide bind would expose every document to the network (M3.4).
static MYSQL_SYSVAR_STR(bind, chimera_mongo_bind_value,
                        PLUGIN_VAR_READONLY | PLUGIN_VAR_MEMALLOC,
                        "IPv4 address the MongoDB wire listener binds to",
                        nullptr, nullptr, "127.0.0.1");

// run-server.sh always passes the port explicitly, because the two server
// versions run side by side and must not collide. The default matches 11.8.
static MYSQL_SYSVAR_UINT(port, chimera_mongo_port_value, PLUGIN_VAR_READONLY,
                         "TCP port the MongoDB wire listener accepts on",
                         nullptr, nullptr, 27018, 1, 65535, 0);

// The oplog is a capped collection in behaviour, not in storage, so the cap is
// two settable limits. 100k entries is roughly a day of a small application's
// writes; the age limit is what actually bounds recovery for an idle system.
// Zero switches either half off.
static MYSQL_SYSVAR_ULONGLONG(oplog_max_rows, chimera_mongo_oplog_max_rows_value, 0,
                              "Maximum oplog entries retained (0 disables the limit)",
                              nullptr, nullptr, 100000, 0, ~0ULL, 1);

static MYSQL_SYSVAR_ULONGLONG(oplog_max_age_seconds, chimera_mongo_oplog_max_age_value, 0,
                              "Maximum oplog entry age in seconds (0 disables the limit)",
                              nullptr, nullptr, 86400, 0, ~0ULL, 1);

// The SQL gateway lets a mongo client run SQL. Off for writes by default: a
// mongo connection is not authenticated yet (M3.4), so anything it can reach
// should be something it could already have read through a collection.
static MYSQL_SYSVAR_BOOL(sql_writes, chimera_mongo_sql_writes_value, 0,
                         "Allow chimeraSql and the $sql stage to run statements that write",
                         nullptr, nullptr, 0);

static struct st_mysql_sys_var *chimera_mongo_system_variables[] = {
  MYSQL_SYSVAR(bind),
  MYSQL_SYSVAR(port),
  MYSQL_SYSVAR(oplog_max_rows),
  MYSQL_SYSVAR(oplog_max_age_seconds),
  MYSQL_SYSVAR(sql_writes),
  nullptr
};

static int chimera_mongo_init(void *p)
{
  chimera::set_sql_gateway_write_flag(&chimera_mongo_sql_writes_value);

  auto listener = std::make_unique<chimera::Listener>(
      chimera_mongo_bind_value ? chimera_mongo_bind_value : "127.0.0.1",
      static_cast<uint16_t>(chimera_mongo_port_value));

  std::string error;
  if (!listener->start(&error))
  {
    fprintf(stderr, "chimera_mongo: failed to start listener: %s\n", error.c_str());
    return 1;
  }

  fprintf(stderr, "chimera_mongo: listening on %s:%u\n",
          chimera_mongo_bind_value, chimera_mongo_port_value);

  chimera_mongo_pruner= std::make_unique<chimera::OplogPruner>(
      &chimera_mongo_oplog_max_rows_value, &chimera_mongo_oplog_max_age_value);

  static_cast<struct st_plugin_int *>(p)->data= listener.release();
  return 0;
}

static int chimera_mongo_deinit(void *p)
{
  auto *plugin= static_cast<struct st_plugin_int *>(p);
  // Stop the pruner before the listener: it holds a session of its own, and
  // joining it first means no thread outlives the plugin's SQL access.
  chimera_mongo_pruner.reset();
  // The destructor stops accepting, unblocks every connection thread and joins
  // them, so the server shuts down with no threads left behind.
  delete static_cast<chimera::Listener *>(plugin->data);
  plugin->data= nullptr;
  return 0;
}

static struct st_mysql_daemon chimera_mongo_plugin_info=
{ MYSQL_DAEMON_INTERFACE_VERSION };

maria_declare_plugin(chimera_mongo)
{
  MYSQL_DAEMON_PLUGIN,
  &chimera_mongo_plugin_info,
  "chimera_mongo",
  "ChimeraDB",
  "MongoDB wire protocol listener for MariaDB",
  PLUGIN_LICENSE_GPL,
  chimera_mongo_init,
  chimera_mongo_deinit,
  0x0100 /* 1.0 */,
  nullptr,                              /* status variables */
  chimera_mongo_system_variables,       /* system variables */
  "1.0",
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
}
maria_declare_plugin_end;
