#include "chimera/sqlguard.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace chimera {
namespace {

bool starts_with(std::string_view text, size_t at, std::string_view prefix) {
  return text.compare(at, prefix.size(), prefix) == 0;
}

// Advances past whitespace and every form of comment MariaDB accepts before a
// statement's first keyword.
size_t skip_noise(std::string_view text) {
  size_t i = 0;
  while (i < text.size()) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (std::isspace(c)) {
      ++i;
    } else if (starts_with(text, i, "/*")) {
      const size_t end = text.find("*/", i + 2);
      if (end == std::string_view::npos) return text.size();
      i = end + 2;
    } else if (text[i] == '#' || starts_with(text, i, "--")) {
      const size_t end = text.find('\n', i);
      if (end == std::string_view::npos) return text.size();
      i = end + 1;
    } else {
      break;
    }
  }
  return i;
}

}  // namespace

std::string leading_keyword(std::string_view statement) {
  size_t i = skip_noise(statement);
  std::string word;
  while (i < statement.size() && std::isalpha(static_cast<unsigned char>(statement[i]))) {
    word.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(statement[i]))));
    ++i;
  }
  return word;
}

bool is_read_only_statement(std::string_view statement) {
  // `WITH` is absent deliberately: MariaDB allows a common table expression in
  // front of UPDATE and DELETE, so the first keyword stops being evidence.
  static constexpr std::array<std::string_view, 5> kReadOnly = {
      "SELECT", "SHOW", "DESCRIBE", "DESC", "EXPLAIN"};
  const std::string keyword = leading_keyword(statement);
  return std::find(kReadOnly.begin(), kReadOnly.end(), keyword) != kReadOnly.end();
}

}  // namespace chimera
