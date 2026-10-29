#pragma once
#include "scram.h"
#include "sql.h"
#include "chimera/bson.h"

namespace chimera {
std::optional<ScramCredential> find_credential(SqlSession& sql, const std::string& db,
                                              const std::string& user);
class Authentication {
public:
  Bson start(const bson_t* body, const std::string& db, SqlSession& sql);
  Bson more(const bson_t* body, const std::string& db, SqlSession& sql);
  void validate(SqlSession& sql);
  bool authenticated() const { return authenticated_; }
  const ScramCredential& credential() const { return credential_; }
  std::string principal() const;
  void reset();
private:
  ScramConversation conversation_;
  ScramCredential credential_;
  int32_t conversation_id_ = 0;
  bool authenticated_ = false, awaiting_empty_ = false;
};
}  // namespace chimera
