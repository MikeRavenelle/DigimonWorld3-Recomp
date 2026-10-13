// Command lines of the programs the wizard runs. Pure functions: nothing here starts a process.
#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace dw3setup {

// `curl --version`: the check that curl exists (it ships with Windows 10+, macOS and the Linux distributions).
std::vector<std::string> curl_version_command();
// Downloads `url` to `destination`: follows redirects, fails on HTTP errors, retries transient failures.
std::vector<std::string> curl_download_command(const std::string& url, const std::filesystem::path& destination);

// A player-readable reason for a curl exit code (curl's documented codes).
std::string describe_curl_exit(int code);

}  // namespace dw3setup
