#include "dw3/game_module.h"

#include "dw3/platform.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace dw3 {

const Dw3Api& GameModule::api() {
    static const Dw3Api table = {
        DW3_API_VERSION, dw3_mem_read,       dw3_mem_write,       dw3_lookup_func,     dw3_service_events,
        dw3_syscall,     dw3_break,          dw3_bad_jump,        dw3_bios_call,       dw3_bad_resume,
        dw3_mtc0,        dw3_rfe,            dw3_gte_read_data,   dw3_gte_write_data,  dw3_gte_read_ctrl,
        dw3_gte_write_ctrl, dw3_gte_cmd};
    return table;
}

std::unique_ptr<GameModule> GameModule::load(const std::filesystem::path& path, const std::string& expected_build_id,
                                             std::string& error) {
    std::unique_ptr<GameModule> m(new GameModule());
    const std::string shown = platform::path_to_utf8(path);
    RecompModuleInit init = nullptr;
#if defined(_WIN32)
    m->handle_ = LoadLibraryW(path.c_str());
    if (m->handle_) init = reinterpret_cast<RecompModuleInit>(GetProcAddress(static_cast<HMODULE>(m->handle_), DW3_MODULE_INIT_SYMBOL));
#else
    m->handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (m->handle_) init = reinterpret_cast<RecompModuleInit>(dlsym(m->handle_, DW3_MODULE_INIT_SYMBOL));
#endif
    if (!m->handle_) {
        error = "cannot load game module " + shown;
        return nullptr;
    }
    if (!init) {
        error = shown + " is not a game module";
        return nullptr;
    }
    m->info_ = init(&api());
    if (!m->info_ || m->info_->api_version != DW3_API_VERSION || !m->info_->segments) {
        error = shown + " was built for another version of the game runtime";
        return nullptr;
    }
    m->build_id_ = m->info_->build_id ? m->info_->build_id : "";
    if (!expected_build_id.empty() && m->build_id_ != expected_build_id) {
        error = shown + " was built from other recompiler output (" + m->build_id_ + ", expected " + expected_build_id + ")";
        return nullptr;
    }
    return m;
}

GameModule::~GameModule() {
    if (!handle_) return;
#if defined(_WIN32)
    FreeLibrary(static_cast<HMODULE>(handle_));
#else
    dlclose(handle_);
#endif
}

}  // namespace dw3
