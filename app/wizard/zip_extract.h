// Unpacks the upscaler's release archive.
#pragma once
#include <filesystem>
#include <string>

namespace dw3setup {

// Extracts every file of `zip` into `destination`, dropping the single top-level folder the archive wraps its files in
// (if it has one). Entries that would leave `destination` are refused. Files whose archive mode marks them executable
// get the executable bit.
bool extract_zip(const std::filesystem::path& zip, const std::filesystem::path& destination, std::string& error);

}  // namespace dw3setup
