#include "module_build.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <thread>

#include "psxdisc/disc.h"
#include "psxrecomp/pipeline.h"
#include "process_runner.h"
#include "sha256.h"

namespace fs = std::filesystem;
using dw3::platform::Arch;
using dw3::platform::Os;

namespace dw3setup {

namespace {

constexpr const char* kCompilerName =
#if defined(DW3_OS_windows)
    "zig.exe";
#else
    "zig";
#endif

std::string utf8(const fs::path& p) { return dw3::platform::path_to_utf8(p); }

bool path_exists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

std::optional<Toolchain> toolchain_in(const fs::path& dir) {
    Toolchain t{dir / kCompilerName, dir / "include", dir / "module" / "module_entry.c"};
    if (path_exists(t.compiler) && path_exists(t.include_dir / "dw3" / "recomp.h") && path_exists(t.module_entry)) return t;
    return std::nullopt;
}

}  // namespace

std::optional<Toolchain> find_toolchain(const fs::path& app_dir) {
    if (const char* env = std::getenv("DW3_TOOLCHAIN"); env && *env)
        if (auto t = toolchain_in(dw3::platform::path_from_utf8(env))) return t;
    return toolchain_in(app_dir / "toolchain");
}

std::string compile_target(Os os, Arch arch) {
    const char* cpu = arch == Arch::X86_64 ? "x86_64" : "aarch64";
    switch (os) {
        case Os::Linux: return std::string(cpu) + "-linux-gnu";
        case Os::Windows: return std::string(cpu) + "-windows-gnu";
        case Os::MacOS: return std::string(cpu) + "-macos";
    }
    return {};
}

std::string module_extension(Os os) { return os == Os::Windows ? "dll" : os == Os::MacOS ? "dylib" : "so"; }

std::string make_build_id(const std::string& version, const std::string& game_config, const std::string& exe_sha256) {
    const std::string text = version + "|" + game_config + "|" + exe_sha256;
    const auto* bytes = reinterpret_cast<const uint8_t*>(text.data());
    return sha256_hex({bytes, text.size()}).substr(0, 16);
}

fs::path module_path(const fs::path& user_data, const std::string& build_id, Os os) {
    return user_data / "module" / ("dw3game-" + build_id + "." + module_extension(os));
}

PrepareSnapshot ModulePreparer::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

void ModulePreparer::set_status(PreparePhase phase, std::string status, double fraction) {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_ = {phase, std::move(status), fraction};
}

PrepareResult ModulePreparer::fail(std::string message) {
    PrepareResult r;
    r.cancelled = cancel_.load();
    r.error = r.cancelled ? "Cancelled." : std::move(message);
    set_status(PreparePhase::Failed, r.error);
    return r;
}

std::vector<std::string> ModulePreparer::compile_command(const fs::path& source, const fs::path& object) const {
    std::vector<std::string> c = {utf8(config_.toolchain.compiler), "cc", "-target", compile_target(config_.os, config_.arch),
                                  "-O1", "-w", "-fexceptions", "-ffp-contract=off", "-DDW3_MODULE=1"};
    if (config_.os != Os::Windows) c.push_back("-fPIC");
    c.insert(c.end(), {"-I", utf8(config_.toolchain.include_dir), "-c", utf8(source), "-o", utf8(object)});
    return c;
}

std::vector<std::string> ModulePreparer::link_command(const fs::path& output, const fs::path& response_file) const {
    return {utf8(config_.toolchain.compiler), "cc", "-target", compile_target(config_.os, config_.arch),
            config_.os == Os::MacOS ? "-dynamiclib" : "-shared", "-o", utf8(output), "@" + utf8(response_file)};
}

bool ModulePreparer::compile_all(const fs::path& dir, const std::vector<std::string>& sources, std::vector<fs::path>& objects,
                                 std::string& error) {
    objects.clear();
    for (const auto& s : sources) objects.push_back(dir / "obj" / (fs::path(s).filename().string() + ".o"));
    std::error_code ec;
    fs::create_directories(dir / "obj", ec);
    std::atomic<std::size_t> next{0}, done{0};
    std::atomic<bool> failed{false};
    std::mutex error_mutex;
    const std::size_t total = sources.size();
    auto worker = [&] {
        for (;;) {
            const std::size_t i = next.fetch_add(1);
            if (i >= total || failed || cancel_) return;
            const int code = config_.run(compile_command(dir / sources[i], objects[i]), cancel_);
            if (code != 0) {
                std::lock_guard<std::mutex> lock(error_mutex);
                if (!failed.exchange(true))
                    error = code == kProcessCancelled ? "" : "The compiler failed on " + sources[i] + " (exit code " + std::to_string(code) + ").";
                return;
            }
            const std::size_t n = done.fetch_add(1) + 1;
            set_status(PreparePhase::Compiling, "Compiling the game code (" + std::to_string(n) + " of " + std::to_string(total) + ")",
                       static_cast<double>(n) / static_cast<double>(total));
        }
    };
    std::vector<std::thread> pool;
    const unsigned n = std::max(1u, std::min<unsigned>(config_.jobs, static_cast<unsigned>(std::max<std::size_t>(total, 1))));
    for (unsigned k = 0; k < n; ++k) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
    return !failed && !cancel_ && done == total;
}

PrepareResult ModulePreparer::run() {
    const fs::path out = module_path(config_.user_data, config_.build_id, config_.os);
    if (path_exists(out)) {
        PrepareResult r;
        r.ok = true;
        r.module = out;
        set_status(PreparePhase::Linking, "The game is already prepared", 1.0);
        return r;
    }
    const fs::path dir = config_.user_data / "module" / "build";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    if (ec) return fail("Cannot create " + utf8(dir) + ".");

    set_status(PreparePhase::Reading, "Reading the disc");
    auto disc = psxdisc::Disc::open(config_.disc);
    if (!disc) return fail("Cannot open the disc image.");
    toml::table game_config;
    try {
        game_config = toml::parse(config_.game_config);
    } catch (const toml::parse_error& e) {
        return fail(std::string("The built-in game configuration is invalid: ") + e.what());
    }
    psxrecomp::Game game;
    std::string error;
    if (!game.load(game_config, psxrecomp::disc_source(*disc), error)) return fail(error + ".");

    psxrecomp::Progress progress;
    progress.cancel = &cancel_;
    progress.report = [this](const std::string& stage, std::size_t done, std::size_t total) {
        const bool writing = stage == "Writing code";
        set_status(writing ? PreparePhase::Writing : PreparePhase::Analysing,
                   writing ? "Translating the game code" : "Analysing the game code", static_cast<double>(done) / static_cast<double>(total));
    };
    set_status(PreparePhase::Analysing, "Analysing the game code", 0.0);
    if (!game.analyze(progress)) return fail("");
    psxrecomp::EmitStats stats;
    if (!game.emit(dir, progress, stats, error)) return fail(error);
    if (stats.functions_with_errors != 0) return fail("The recompiler reported errors in this disc's code.");

    {
        std::ofstream id(dir / "build_id.c");
        id << "const char dw3_module_build_id[] = \"" << config_.build_id << "\";\n";
    }
    std::vector<std::string> sources = stats.sources;
    sources.push_back("build_id.c");
    std::vector<fs::path> objects;
    set_status(PreparePhase::Compiling, "Compiling the game code", 0.0);
    if (!compile_all(dir, sources, objects, error)) return fail(error);
    // module_entry.c comes with the toolchain; build it beside the generated sources.
    {
        const fs::path entry_object = dir / "obj" / "module_entry.c.o";
        if (config_.run(compile_command(config_.toolchain.module_entry, entry_object), cancel_) != 0) return fail("The compiler failed on the module entry point.");
        objects.push_back(entry_object);
    }

    set_status(PreparePhase::Linking, "Linking the game", -1.0);
    const fs::path response = dir / "objects.rsp";
    {
        std::ofstream rsp(response);
        for (const auto& o : objects) rsp << '"' << utf8(o) << "\"\n";
    }
    fs::create_directories(out.parent_path(), ec);
    const fs::path partial = out.parent_path() / (out.filename().string() + ".part");
    if (config_.run(link_command(partial, response), cancel_) != 0) return fail("Linking the game failed.");
    fs::rename(partial, out, ec);
    if (ec) return fail("Cannot store the prepared game in " + utf8(out.parent_path()) + ".");
    fs::remove_all(dir, ec);

    PrepareResult r;
    r.ok = true;
    r.module = out;
    set_status(PreparePhase::Linking, "Done", 1.0);
    return r;
}

}  // namespace dw3setup
