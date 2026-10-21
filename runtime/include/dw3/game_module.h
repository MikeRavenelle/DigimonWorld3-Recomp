// A game module: the recompiled code built into a shared library, loaded at run time (see recomp.h, Dw3Api).
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

#include "dw3/recomp.h"

namespace dw3 {

class GameModule {
public:
    // Loads the library, hands it the runtime's services and reads its segment table. On failure returns null and sets
    // `error`. If `expected_build_id` is not empty, a module built from other output is rejected.
    static std::unique_ptr<GameModule> load(const std::filesystem::path& path, const std::string& expected_build_id,
                                            std::string& error);
    ~GameModule();
    GameModule(const GameModule&) = delete;
    GameModule& operator=(const GameModule&) = delete;

    std::span<const RecompSegment> segments() const { return {info_->segments, info_->segment_count}; }
    const std::string& build_id() const { return build_id_; }

    // The services a module receives.
    static const Dw3Api& api();

private:
    GameModule() = default;
    void* handle_ = nullptr;
    const RecompModuleInfo* info_ = nullptr;
    std::string build_id_;
};

}  // namespace dw3
