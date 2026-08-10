#pragma once

#include "sql.h"

namespace chimera {

// Installs `chimera_meta.chimera_add_projection`, the SQL-facing way to expose a
// document field as a real column. Idempotent; called from Collection::bootstrap
// so a DBA always finds it there.
void install_projection_support(SqlSession& sql);

}  // namespace chimera
