#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace chimera {
using ScramKey = std::array<unsigned char, 32>;
struct ScramCredential {
  std::string auth_db, user, sql_host, revision, sql_role;
  std::string salt;  // raw bytes
  int iterations = 15000;
  ScramKey stored_key{}, server_key{};
};
std::string base64_encode(const std::string& bytes);
std::string base64_decode(const std::string& text);
std::string random_bytes(size_t count);
std::string hex_encode(const std::string& bytes);
std::string hex_decode(const std::string& text);

// RFC 5802 / RFC 7677 server exchange. No database, BSON, or server internals.
// Parsing is deliberately bounded and rejects duplicate/reserved attributes.
class ScramConversation {
public:
  ~ScramConversation() { reset(); }
  std::string start(const std::string& payload, const ScramCredential& credential,
                    bool known_user, const std::string& server_nonce = "");
  std::string finish(const std::string& payload);
  static std::string username(const std::string& payload);
  void reset();
private:
  ScramCredential credential_;
  std::string first_, server_first_, nonce_, channel_binding_;
  bool active_ = false, known_user_ = false;
};
}  // namespace chimera
