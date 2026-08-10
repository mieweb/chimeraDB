#pragma once

#include <stdexcept>
#include <string>

namespace chimera {

// Errors carry MongoDB's numeric code and codeName so the wire layer can hand
// them straight back to a driver without a second mapping table. The codes are
// the publicly documented ones; nothing here is derived from MongoDB source.
class TranslatorError : public std::runtime_error {
public:
  TranslatorError(int code, std::string code_name, const std::string& message)
      : std::runtime_error(message), code_(code), code_name_(std::move(code_name)) {}

  int code() const noexcept { return code_; }
  const std::string& code_name() const noexcept { return code_name_; }

private:
  int code_;
  std::string code_name_;
};

TranslatorError bad_value(const std::string& message);
TranslatorError failed_to_parse(const std::string& message);
TranslatorError type_mismatch(const std::string& message);
TranslatorError not_implemented(const std::string& message);

}  // namespace chimera
