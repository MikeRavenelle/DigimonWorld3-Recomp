// Prepares the game from the player's own disc: recompile it to C with the library recompiler, compile the C into a
// game module with the toolchain that ships beside the program, and store the module in the user data directory. The
// program contains no game code, so this runs once per disc and recompiler version. Processes are injected so that the
// sequence can be tested; run() blocks and is meant for a worker thread.
#pragma once
#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "dw3/platform.h"

namespace dw3setup {

// The compiler and the files module code is built from. Shipped with the program.
struct Toolchain {
    std::filesystem::path compiler;      // zig (used as `zig cc`)
    std::filesystem::path include_dir;   // contains dw3/recomp.h
    std::filesystem::path module_entry;  // module_entry.c
};

// Looks beside the executable (<app_dir>/toolchain) and in the folder named by DW3_TOOLCHAIN. Empty when incomplete.
std::optional<Toolchain> find_toolchain(const std::filesystem::path& app_dir);

// The compiler's name for the platform the program runs on, e.g. "x86_64-linux-gnu".
std::string compile_target(dw3::platform::Os os, dw3::platform::Arch arch);
std::string module_extension(dw3::platform::Os os);  // "so", "dll", "dylib"

// Identifies what a module was built from: the program version, the recompiler configuration and the disc executable.
std::string make_build_id(const std::string& version, const std::string& game_config, const std::string& exe_sha256);
std::filesystem::path module_path(const std::filesystem::path& user_data, const std::string& build_id, dw3::platform::Os os);

enum class PreparePhase { Reading, Analysing, Writing, Compiling, Linking, Failed };

struct PrepareConfig {
    std::filesystem::path user_data;
    std::filesystem::path disc;
    std::string game_config;  // the recompiler configuration (TOML text)
    Toolchain toolchain;
    dw3::platform::Os os = dw3::platform::host_os();
    dw3::platform::Arch arch = dw3::platform::host_arch();
    std::string build_id;
    unsigned jobs = 4;
    // Runs a program and returns its exit code (see process_runner.h for the negative codes).
    std::function<int(const std::vector<std::string>&, const std::atomic<bool>& cancel)> run;
};

struct PrepareSnapshot {
    PreparePhase phase = PreparePhase::Reading;
    std::string status;      // one line: what is happening now
    double fraction = -1.0;  // 0..1 of the current phase, or -1 when unknown
};

struct PrepareResult {
    bool ok = false;
    bool cancelled = false;
    std::string error;
    std::filesystem::path module;
};

class ModulePreparer {
public:
    explicit ModulePreparer(PrepareConfig config) : config_(std::move(config)) {}
    PrepareResult run();
    void cancel() { cancel_ = true; }
    PrepareSnapshot snapshot() const;

private:
    void set_status(PreparePhase phase, std::string status, double fraction = -1.0);
    PrepareResult fail(std::string message);
    bool compile_all(const std::filesystem::path& dir, const std::vector<std::string>& sources, std::vector<std::filesystem::path>& objects, std::string& error);
    std::vector<std::string> compile_command(const std::filesystem::path& source, const std::filesystem::path& object) const;
    std::vector<std::string> link_command(const std::filesystem::path& output, const std::filesystem::path& response_file) const;

    PrepareConfig config_;
    std::atomic<bool> cancel_{false};
    mutable std::mutex mutex_;
    PrepareSnapshot snapshot_;
};

}  // namespace dw3setup
