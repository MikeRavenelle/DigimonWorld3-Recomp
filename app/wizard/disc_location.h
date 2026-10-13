// Where the disc image comes from when the program starts.
#pragma once
#include <functional>
#include <optional>
#include <string>

namespace dw3setup {

// Order: the --disc flag (taken as given), the path stored in settings.toml (system.disc_path), then the folder-relative
// default used when running from a source checkout. The stored path and the default must pass `is_valid`.
// nullopt means no usable disc is configured and the setup wizard should run.
std::optional<std::string> locate_disc(const std::string& flag, const std::string& stored, const std::string& checkout_default,
                                       const std::function<bool(const std::string&)>& is_valid);

}  // namespace dw3setup
