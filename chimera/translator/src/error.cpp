#include "chimera/error.h"

namespace chimera {

// The public MongoDB error codes the translator itself can raise.
TranslatorError bad_value(const std::string& message) { return {2, "BadValue", message}; }
TranslatorError failed_to_parse(const std::string& message) { return {9, "FailedToParse", message}; }
TranslatorError type_mismatch(const std::string& message) { return {14, "TypeMismatch", message}; }
TranslatorError not_implemented(const std::string& message) {
  return {238, "NotImplemented", message};
}

}  // namespace chimera
