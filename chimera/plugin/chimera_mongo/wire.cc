#include "wire.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "chimera/error.h"

namespace chimera {
namespace wire {
namespace {

int32_t read_int32(const uint8_t* p) {
  return static_cast<int32_t>(static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                              (static_cast<uint32_t>(p[2]) << 16) |
                              (static_cast<uint32_t>(p[3]) << 24));
}

void append_int32(std::vector<uint8_t>& out, int32_t v) {
  uint32_t u = static_cast<uint32_t>(v);
  out.push_back(static_cast<uint8_t>(u));
  out.push_back(static_cast<uint8_t>(u >> 8));
  out.push_back(static_cast<uint8_t>(u >> 16));
  out.push_back(static_cast<uint8_t>(u >> 24));
}

void append_int64(std::vector<uint8_t>& out, int64_t v) {
  append_int32(out, static_cast<int32_t>(v));
  append_int32(out, static_cast<int32_t>(v >> 32));
}

bool read_exact(int fd, uint8_t* buf, size_t want) {
  size_t got = 0;
  while (got < want) {
    ssize_t n = ::recv(fd, buf + got, want - got, 0);
    if (n == 0) return false;  // peer closed
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    got += static_cast<size_t>(n);
  }
  return true;
}

// A bounds-checked cursor over the message body. Every read is validated
// because the bytes come straight off an untrusted socket.
class Cursor {
public:
  Cursor(const uint8_t* data, size_t size) : data_(data), size_(size) {}

  int32_t int32_at_cursor() {
    require(4);
    int32_t v = read_int32(data_ + at_);
    at_ += 4;
    return v;
  }

  uint8_t byte() {
    require(1);
    return data_[at_++];
  }

  std::string cstring() {
    size_t end = at_;
    while (end < size_ && data_[end] != 0) ++end;
    if (end >= size_) throw failed_to_parse("unterminated string in wire message");
    std::string s(reinterpret_cast<const char*>(data_ + at_), end - at_);
    at_ = end + 1;
    return s;
  }

  Bson document() {
    require(4);
    int32_t len = read_int32(data_ + at_);
    if (len < 5) throw failed_to_parse("BSON document length is impossible");
    require(static_cast<size_t>(len));
    bson_t* doc = bson_new_from_data(data_ + at_, static_cast<size_t>(len));
    if (!doc) throw failed_to_parse("malformed BSON document in wire message");
    at_ += static_cast<size_t>(len);
    return Bson(doc);
  }

  size_t remaining() const { return size_ - at_; }
  void skip(size_t n) {
    require(n);
    at_ += n;
  }
  void trim(size_t n) {
    if (size_ - at_ < n) throw failed_to_parse("wire message is shorter than it claims");
    size_ -= n;
  }

private:
  const uint8_t* data_;
  size_t size_;
  size_t at_ = 0;

  void require(size_t n) const {
    if (size_ - at_ < n) throw failed_to_parse("wire message truncated");
  }
};

}  // namespace

bool read_message(int fd, std::vector<uint8_t>& raw) {
  uint8_t header[kHeaderBytes];
  if (!read_exact(fd, header, sizeof header)) return false;

  int32_t length = read_int32(header);
  if (length < kHeaderBytes || length > kMaxMessageBytes) {
    throw failed_to_parse("message length " + std::to_string(length) + " is out of range");
  }

  raw.assign(header, header + kHeaderBytes);
  raw.resize(static_cast<size_t>(length));
  if (length > kHeaderBytes &&
      !read_exact(fd, raw.data() + kHeaderBytes, static_cast<size_t>(length) - kHeaderBytes)) {
    return false;
  }
  return true;
}

Header parse_header(const std::vector<uint8_t>& raw) {
  Header header;
  header.message_length = read_int32(raw.data());
  header.request_id = read_int32(raw.data() + 4);
  header.response_to = read_int32(raw.data() + 8);
  header.op_code = read_int32(raw.data() + 12);
  return header;
}

Request parse_request(const std::vector<uint8_t>& raw) {
  Request req;
  req.header = parse_header(raw);

  Cursor cursor(raw.data() + kHeaderBytes, raw.size() - kHeaderBytes);

  if (req.header.op_code == kOpMsg) {
    int32_t flags = cursor.int32_at_cursor();
    // Bits 0-15 are "required to understand"; we implement checksumPresent (0)
    // and moreToCome (1). Anything else must be refused rather than ignored.
    if ((flags & 0xFFFF & ~0x3) != 0) {
      throw failed_to_parse("OP_MSG sets flag bits this server does not implement");
    }
    if (flags & 0x1) cursor.trim(4);  // trailing CRC-32C, which we do not verify

    uint8_t kind = cursor.byte();
    if (kind != 0) {
      // Document sequences carry bulk payloads; nothing in the handshake uses
      // them, so refuse loudly instead of silently dropping the data.
      throw not_implemented("OP_MSG document sequences are not supported yet");
    }
    req.body = cursor.document();
    auto db = path_get(req.body.get(), {"$db"});
    if (db && db->type() == BSON_TYPE_UTF8) req.database = db->get().value.v_utf8.str;
    return req;
  }

  if (req.header.op_code == kOpQuery) {
    // Drivers and the legacy shell send their very first handshake this way,
    // before they know the server speaks OP_MSG.
    req.legacy_query = true;
    cursor.int32_at_cursor();  // flags
    std::string ns = cursor.cstring();
    cursor.int32_at_cursor();  // numberToSkip
    cursor.int32_at_cursor();  // numberToReturn
    req.body = cursor.document();
    req.database = ns.substr(0, ns.find('.'));
    return req;
  }

  throw not_implemented("opcode " + std::to_string(req.header.op_code) + " is not supported");
}

std::vector<uint8_t> encode_reply(const Request& req, int32_t request_id, const bson_t* body) {
  const uint8_t* doc = bson_get_data(body);
  size_t doc_len = body->len;

  std::vector<uint8_t> out;
  append_int32(out, 0);  // placeholder for messageLength
  append_int32(out, request_id);
  append_int32(out, req.header.request_id);

  if (req.legacy_query) {
    append_int32(out, kOpReply);
    append_int32(out, 0);  // responseFlags
    append_int64(out, 0);  // cursorId
    append_int32(out, 0);  // startingFrom
    append_int32(out, 1);  // numberReturned
  } else {
    append_int32(out, kOpMsg);
    append_int32(out, 0);  // flagBits
    out.push_back(0);      // section kind 0: a single body document
  }
  out.insert(out.end(), doc, doc + doc_len);

  int32_t total = static_cast<int32_t>(out.size());
  out[0] = static_cast<uint8_t>(total);
  out[1] = static_cast<uint8_t>(total >> 8);
  out[2] = static_cast<uint8_t>(total >> 16);
  out[3] = static_cast<uint8_t>(total >> 24);
  return out;
}

bool write_all(int fd, const std::vector<uint8_t>& bytes) {
  size_t sent = 0;
  while (sent < bytes.size()) {
    ssize_t n = ::send(fd, bytes.data() + sent, bytes.size() - sent, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    sent += static_cast<size_t>(n);
  }
  return true;
}

}  // namespace wire
}  // namespace chimera
