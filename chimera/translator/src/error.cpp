#include "chimera/error.h"

namespace chimera {

// The public MongoDB error codes the translator itself can raise.
TranslatorError bad_value(const std::string& message) { return {2, "BadValue", message}; }
TranslatorError failed_to_parse(const std::string& message) { return {9, "FailedToParse", message}; }
TranslatorError missing_command_field(const std::string& message) {
  return {40414, "IDLFailedToParse", message};
}
TranslatorError type_mismatch(const std::string& message) { return {14, "TypeMismatch", message}; }
TranslatorError not_implemented(const std::string& message) {
  return {238, "NotImplemented", message};
}
TranslatorError namespace_not_found(const std::string& message) {
  return {26, "NamespaceNotFound", message};
}
TranslatorError namespace_exists(const std::string& message) {
  return {48, "NamespaceExists", message};
}
TranslatorError index_not_found(const std::string& message) {
  return {27, "IndexNotFound", message};
}
TranslatorError duplicate_key(const std::string& message) {
  return {11000, "DuplicateKey", message};
}
TranslatorError cursor_not_found(const std::string& message) {
  return {43, "CursorNotFound", message};
}
TranslatorError internal_error(const std::string& message) {
  return {1, "InternalError", message};
}

}  // namespace chimera
