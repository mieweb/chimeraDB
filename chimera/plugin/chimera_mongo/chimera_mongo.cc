// The only file in ChimeraDB that includes MariaDB headers. It exists to
// declare the daemon plugin and hand the configured address and port to the
// listener; everything else about the Mongo head lives in server-agnostic code
// so it can be built and reasoned about without a server.
#include <my_global.h>
#include <mysql_version.h>
#include <mysql/plugin.h>

#include <memory>
#include <string>

#include "chimera/sqlguard.h"
#include "listener.h"
#include "oplog.h"
#include "sqlgateway.h"

static char *chimera_mongo_bind_value = nullptr;
static unsigned int chimera_mongo_port_value = 0;
static unsigned long long chimera_mongo_oplog_max_rows_value = 0;
static unsigned long long chimera_mongo_oplog_max_age_value = 0;
static char chimera_mongo_sql_writes_value = 0;
static char chimera_mongo_insecure_bind_value = 0;
static char chimera_mongo_auth_value = 1;
static char *chimera_mongo_tls_cert_value = nullptr;
static char *chimera_mongo_tls_key_value = nullptr;
static MYSQL_SYSVAR_BOOL(auth, chimera_mongo_auth_value, PLUGIN_VAR_READONLY,
                         "Require SCRAM-SHA-256 authentication (default ON)", nullptr, nullptr, 1);
static MYSQL_SYSVAR_STR(tls_cert, chimera_mongo_tls_cert_value,
                        PLUGIN_VAR_READONLY | PLUGIN_VAR_MEMALLOC,
                        "PEM server certificate chain; when set every Mongo connection requires TLS",
                        nullptr, nullptr, nullptr);
static MYSQL_SYSVAR_STR(tls_key, chimera_mongo_tls_key_value,
                        PLUGIN_VAR_READONLY | PLUGIN_VAR_MEMALLOC,
                        "PEM private key for the Mongo TLS certificate", nullptr, nullptr, nullptr);
// A daemon plugin is a singleton, so file scope holds these as well as the
// plugin handle would — and without <sql_plugin.h>, which is server-internal and
// not shipped in any -dev package (D11).
static std::unique_ptr<chimera::Listener> chimera_mongo_listener;
static std::unique_ptr<chimera::OplogPruner> chimera_mongo_pruner;

// Loopback by default and, since #5 stage 1, enforced: init refuses a
// non-loopback bind unless chimera_mongo_insecure_bind acknowledges the risk.
static MYSQL_SYSVAR_STR(bind, chimera_mongo_bind_value,
                        PLUGIN_VAR_READONLY | PLUGIN_VAR_MEMALLOC,
                        "IPv4 address the MongoDB wire listener binds to",
                        nullptr, nullptr, "127.0.0.1");

// Explicit opt-in for remote deployments missing authentication or TLS.
static MYSQL_SYSVAR_BOOL(insecure_bind, chimera_mongo_insecure_bind_value,
                         PLUGIN_VAR_READONLY,
                         "Allow a non-loopback Mongo listener without authentication or TLS",
                         nullptr, nullptr, 0);

// run-server.sh always passes the port explicitly, because the two server
// versions run side by side and must not collide. The default matches 11.8.
static MYSQL_SYSVAR_UINT(port, chimera_mongo_port_value, PLUGIN_VAR_READONLY,
                         "TCP port the MongoDB wire listener accepts on",
                         nullptr, nullptr, 27018, 1, 65535, 0);

// The oplog is a capped collection in behaviour, not in storage, so the cap is
// two settable limits. 100k entries is roughly a day of a small application's
// writes; the age limit is what actually bounds recovery for an idle system.
// Zero switches either half off.
//
// Neither limit will delete the newest entry, so the oplog is never empty once
// anything has been written. That is what keeps a change stream's "is my resume
// token still in range?" question answerable rather than a guess; see
// changestream-plan.md CS3.4. Both limits therefore bound how far behind a
// consumer may fall before it is told so — not whether it is told.
static MYSQL_SYSVAR_ULONGLONG(oplog_max_rows, chimera_mongo_oplog_max_rows_value, 0,
                              "Maximum oplog entries retained (0 disables the limit)",
                              nullptr, nullptr, 100000, 0, ~0ULL, 1);

static MYSQL_SYSVAR_ULONGLONG(oplog_max_age_seconds, chimera_mongo_oplog_max_age_value, 0,
                              "Maximum oplog entry age in seconds (0 disables the limit; "
                              "the newest entry is always retained)",
                              nullptr, nullptr, 86400, 0, ~0ULL, 1);

// SQL gateway writes remain an opt-in, in addition to native SQL privileges.
static MYSQL_SYSVAR_BOOL(sql_writes, chimera_mongo_sql_writes_value, 0,
                         "Allow chimeraSql and the $sql stage to run statements that write",
                         nullptr, nullptr, 0);

static struct st_mysql_sys_var *chimera_mongo_system_variables[] = {
  MYSQL_SYSVAR(bind),
  MYSQL_SYSVAR(insecure_bind),
  MYSQL_SYSVAR(auth),
  MYSQL_SYSVAR(tls_cert),
  MYSQL_SYSVAR(tls_key),
  MYSQL_SYSVAR(port),
  MYSQL_SYSVAR(oplog_max_rows),
  MYSQL_SYSVAR(oplog_max_age_seconds),
  MYSQL_SYSVAR(sql_writes),
  nullptr
};

static int chimera_mongo_init(void *)
{
  const char *bind_address =
      chimera_mongo_bind_value ? chimera_mongo_bind_value : "127.0.0.1";
  const bool exposed = !chimera::is_loopback_address(bind_address);
  const bool auth = chimera_mongo_auth_value != 0;
  const std::string cert = chimera_mongo_tls_cert_value ? chimera_mongo_tls_cert_value : "";
  const std::string key = chimera_mongo_tls_key_value ? chimera_mongo_tls_key_value : "";
  if (cert.empty() != key.empty()) {
    fprintf(stderr, "chimera_mongo: tls_cert and tls_key must both be set\n");
    return 1;
  }
  if (exposed && (!auth || cert.empty())) {
    if (!chimera_mongo_insecure_bind_value) {
      fprintf(stderr, "chimera_mongo: refusing non-loopback bind without authentication and TLS; "
                      "chimera_mongo_insecure_bind=ON explicitly overrides this requirement\n");
      return 1;
    }
    fprintf(stderr, "chimera_mongo: WARNING: insecure non-loopback bind: authentication=%s TLS=%s\n",
            auth ? "ON" : "OFF", cert.empty() ? "OFF" : "ON");
  }
  if (!auth)
    fprintf(stderr, "chimera_mongo: WARNING: WITHOUT AUTHENTICATION; anyone reaching the Mongo port "
                    "can read and write every collection\n");
  chimera::set_sql_gateway_write_flag(&chimera_mongo_sql_writes_value);
  chimera::set_sql_gateway_network_exposed(exposed && !auth);
  auto listener = std::make_unique<chimera::Listener>(
      bind_address, static_cast<uint16_t>(chimera_mongo_port_value), auth, cert, key);

  std::string error;
  if (!listener->start(&error))
  {
    fprintf(stderr, "chimera_mongo: failed to start listener: %s\n", error.c_str());
    return 1;
  }

  fprintf(stderr, "chimera_mongo: listening on %s:%u\n",
          bind_address, chimera_mongo_port_value);

  chimera_mongo_pruner= std::make_unique<chimera::OplogPruner>(
      &chimera_mongo_oplog_max_rows_value, &chimera_mongo_oplog_max_age_value);

  chimera_mongo_listener= std::move(listener);
  return 0;
}

static int chimera_mongo_deinit(void *)
{
  // Stop the pruner before the listener: it holds a session of its own, and
  // joining it first means no thread outlives the plugin's SQL access.
  chimera_mongo_pruner.reset();
  // The destructor stops accepting, unblocks every connection thread and joins
  // them, so the server shuts down with no threads left behind.
  chimera_mongo_listener.reset();
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
