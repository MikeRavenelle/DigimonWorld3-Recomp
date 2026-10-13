#include "commands.h"

#include "dw3/platform.h"

namespace dw3setup {

std::vector<std::string> curl_version_command() { return {"curl", "--version"}; }

std::vector<std::string> curl_download_command(const std::string& url, const std::filesystem::path& destination) {
    return {"curl",        "--fail",   "--location", "--silent", "--show-error", "--retry", "3", "--connect-timeout", "20",
            "--output",    dw3::platform::path_to_utf8(destination), "--", url};
}

std::string describe_curl_exit(int code) {
    switch (code) {
        case 0: return "ok";
        case 6: return "the server name could not be resolved (no internet connection?)";
        case 7: return "could not connect to the server";
        case 22: return "the server refused the download (HTTP error; the file may have moved)";
        case 23: return "the file could not be written (disk full or no permission)";
        case 28: return "the connection timed out";
        case 35:
        case 51:
        case 58:
        case 60:
        case 77: return "a secure connection could not be established (check the system date and certificates)";
        default: return "curl failed with exit code " + std::to_string(code);
    }
}

}  // namespace dw3setup
