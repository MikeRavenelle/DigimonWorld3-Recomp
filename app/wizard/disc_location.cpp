#include "disc_location.h"

namespace dw3setup {

std::optional<std::string> locate_disc(const std::string& flag, const std::string& stored, const std::string& checkout_default,
                                       const std::function<bool(const std::string&)>& is_valid) {
    if (!flag.empty()) return flag;
    if (!stored.empty() && is_valid(stored)) return stored;
    if (!checkout_default.empty() && is_valid(checkout_default)) return checkout_default;
    return std::nullopt;
}

}  // namespace dw3setup
