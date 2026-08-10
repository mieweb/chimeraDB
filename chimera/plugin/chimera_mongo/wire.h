#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "chimera/bson.h"

// MongoDB wire protocol framing, written from the public specification. This
// file knows about bytes on a socket and nothing about what commands mean.
namespace chimera {
namespace wire {

constexpr int32_t kOpReply = 1;      // legacy, only ever sent in reply to OP_QUERY
constexpr int32_t kOpQuery = 2004;   // legacy, only accepted for the initial handshake
constexpr int32_t kOpMsg = 2013;
constexpr int32_t kHeaderBytes = 16;
constexpr int32_t kMaxMessageBytes = 48000000;

struct Header {
  int32_t message_length = 0;
  int32_t request_id = 0;
  int32_t response_to = 0;
  int32_t op_code = 0;
};

struct Request {
  Header header;
  Bson body;             // the command document
  std::string database;  // from OP_MSG's $db, or the OP_QUERY namespace
  bool legacy_query = false;  // arrived as OP_QUERY, so it must get an OP_REPLY
};

// Reads exactly one framed message. Returns false on a clean peer close;
// throws on a malformed or oversized frame.
bool read_message(int fd, std::vector<uint8_t>& raw);

// Split out from parse_request so that a reply to an *unparseable* body can
// still carry the right responseTo and reach the client.
Header parse_header(const std::vector<uint8_t>& raw);

Request parse_request(const std::vector<uint8_t>& raw);

std::vector<uint8_t> encode_reply(const Request& req, int32_t request_id, const bson_t* body);

bool write_all(int fd, const std::vector<uint8_t>& bytes);

}  // namespace wire
}  // namespace chimera
