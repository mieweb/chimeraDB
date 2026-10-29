// Clean-room implementation from RFC 5802 and RFC 7677.
#include "scram.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <map>
#include <stdexcept>

namespace chimera {
namespace {
[[noreturn]] void invalid() { throw std::runtime_error("Authentication failed."); }
ScramKey hash(const std::string& input) {
  ScramKey out{};
  unsigned length = 0;
  if (!EVP_Digest(input.data(), input.size(), out.data(), &length, EVP_sha256(), nullptr) ||
      length != out.size()) invalid();
  return out;
}
std::string bytes(const ScramKey& key) {
  return std::string(reinterpret_cast<const char*>(key.data()), key.size());
}
ScramKey hmac(const ScramKey& key, const std::string& input) {
  ScramKey out{};
  unsigned length = 0;
  if (!HMAC(EVP_sha256(), key.data(), key.size(),
            reinterpret_cast<const unsigned char*>(input.data()), input.size(),
            out.data(), &length) || length != out.size()) invalid();
  return out;
}
std::map<char, std::string> attributes(const std::string& input) {
  if (input.empty() || input.size() > 4096 || input.find('\0') != std::string::npos) invalid();
  std::map<char, std::string> out;
  size_t at = 0;
  while (at < input.size()) {
    const size_t end = input.find(',', at);
    const std::string part = input.substr(at, end == std::string::npos ? end : end - at);
    if (part.size() < 3 || part[1] != '=' || part[0] == 'm' ||
        !out.emplace(part[0], part.substr(2)).second) invalid();
    if (end == std::string::npos) break;
    at = end + 1;
    if (at == input.size()) invalid();
  }
  return out;
}
std::string bare_first(const std::string& payload) {
  // No channel binding mechanism is advertised. 'y' is valid for a client that
  // supports channel binding when the server does not; authzid is unsupported.
  if (payload.size() < 3 || (payload.substr(0, 3) != "n,," &&
                            payload.substr(0, 3) != "y,,")) invalid();
  return payload.substr(3);
}
}  // namespace

std::string random_bytes(size_t count) {
  if (count > 4096) invalid();
  std::string out(count, '\0');
  if (RAND_bytes(reinterpret_cast<unsigned char*>(out.data()), static_cast<int>(count)) != 1)
    invalid();
  return out;
}
std::string base64_encode(const std::string& input) {
  std::string out(4 * ((input.size() + 2) / 3) + 1, '\0');
  const int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                               reinterpret_cast<const unsigned char*>(input.data()), input.size());
  out.resize(n);
  return out;
}
std::string base64_decode(const std::string& input) {
  if (input.size() > 8192 || input.size() % 4 != 0) invalid();
  if (input.empty()) return {};
  std::string out(input.size() / 4 * 3, '\0');
  int n = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                          reinterpret_cast<const unsigned char*>(input.data()), input.size());
  if (n < 0) invalid();
  if (input.back() == '=') --n;
  if (input.size() > 1 && input[input.size()-2] == '=') --n;
  if (n < 0) invalid();
  out.resize(n);
  if (base64_encode(out) != input) invalid();
  return out;
}
std::string hex_encode(const std::string& input) {
  static const char digits[] = "0123456789abcdef";
  std::string out;
  for (unsigned char c : input) { out += digits[c >> 4]; out += digits[c & 15]; }
  return out;
}
std::string hex_decode(const std::string& input) {
  if (input.size() % 2) invalid();
  auto digit = [](char c) -> unsigned {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    invalid();
  };
  std::string out;
  for (size_t i = 0; i < input.size(); i += 2)
    out += static_cast<char>(digit(input[i]) * 16 + digit(input[i+1]));
  return out;
}

std::string ScramConversation::username(const std::string& payload) {
  const auto fields = attributes(bare_first(payload));
  if (fields.count('n') != 1 || fields.count('r') != 1 ||
      bare_first(payload).rfind("n=", 0) != 0) invalid();
  const std::string& escaped = fields.at('n');
  std::string user;
  for (size_t i = 0; i < escaped.size(); ++i) {
    if (escaped[i] != '=') { user += escaped[i]; continue; }
    const std::string code = escaped.substr(i, 3);
    if (code == "=2C") user += ',';
    else if (code == "=3D") user += '=';
    else invalid();
    i += 2;
  }
  if (user.empty() || user.size() > 128) invalid();
  return user;
}
void ScramConversation::reset() {
  OPENSSL_cleanse(credential_.stored_key.data(), credential_.stored_key.size());
  OPENSSL_cleanse(credential_.server_key.data(), credential_.server_key.size());
  first_.clear(); server_first_.clear(); nonce_.clear(); channel_binding_.clear();
  active_ = known_user_ = false;
}
std::string ScramConversation::start(const std::string& payload,
                                     const ScramCredential& credential, bool known_user,
                                     const std::string& server_nonce) {
  reset();
  if (username(payload) != credential.user || credential.iterations < 4096 ||
      credential.iterations > 1000000 || credential.salt.size() < 16) invalid();
  first_ = bare_first(payload);
  channel_binding_ = base64_encode(payload.substr(0, 3));
  auto fields = attributes(first_);
  const std::string& client_nonce = fields.at('r');
  if (client_nonce.size() < 16 || client_nonce.size() > 1024) invalid();
  for (unsigned char c : client_nonce) if (c < 0x21 || c > 0x7e || c == ',') invalid();
  nonce_ = client_nonce + (server_nonce.empty() ? base64_encode(random_bytes(24)) : server_nonce);
  credential_ = credential;
  known_user_ = known_user;
  server_first_ = "r=" + nonce_ + ",s=" + base64_encode(credential.salt) +
                  ",i=" + std::to_string(credential.iterations);
  active_ = true;
  return server_first_;
}
std::string ScramConversation::finish(const std::string& payload) {
  if (!active_) invalid();
  active_ = false;  // a conversation may be consumed only once, including failures
  const auto fields = attributes(payload);
  if (fields.count('c') != 1 || fields.count('r') != 1 || fields.count('p') != 1 ||
      fields.at('c') != channel_binding_ || fields.at('r') != nonce_ ||
      payload.rfind("c=", 0) != 0) invalid();
  const size_t proof_at = payload.rfind(",p=");
  if (proof_at == std::string::npos || payload.substr(proof_at + 3) != fields.at('p')) invalid();
  const std::string proof = base64_decode(fields.at('p'));
  if (proof.size() != 32) invalid();
  const std::string message = first_ + "," + server_first_ + "," + payload.substr(0, proof_at);
  ScramKey client_key = hmac(credential_.stored_key, message);
  for (size_t i = 0; i < client_key.size(); ++i) client_key[i] ^= static_cast<unsigned char>(proof[i]);
  const ScramKey stored = hash(bytes(client_key));
  OPENSSL_cleanse(client_key.data(), client_key.size());
  if (CRYPTO_memcmp(stored.data(), credential_.stored_key.data(), stored.size()) != 0 ||
      !known_user_) invalid();
  return "v=" + base64_encode(bytes(hmac(credential_.server_key, message)));
}
}  // namespace chimera
