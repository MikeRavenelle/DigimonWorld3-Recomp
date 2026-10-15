// The whole texture pack job behind `texpack generate`: field maps of a disc (and/or dump folders) -> upscaler -> pack.
// Shared by the texpack tool and the first-run wizard; neither the process nor the console is assumed.
#pragma once
#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "dw3/texrep/generate.h"

namespace dw3::texrep {

// Runs a program with arguments and returns its exit code; a negative value means it could not run or was cancelled.
using CommandRunner = std::function<int(const std::vector<std::string>& command)>;

struct PackJob {
    std::vector<std::filesystem::path> dumps;
    std::vector<std::string> maps;  // only these maps of the disc; empty for all
    std::filesystem::path disc;
    std::filesystem::path out, upscaler, models, work;  // work: temporary files (default: inside the system temporary folder)
    std::string name;
    std::string model = "RealESRGAN_General_x4_v3";
    std::string gpu;
    int scale = 4, model_scale = 4, pad = 16;
    CommandRunner run;
    std::function<void(const std::string&)> log;             // status lines
    std::function<void(std::size_t, std::size_t)> progress;  // images done / total
    std::function<bool()> cancelled;                         // polled between maps and batches; true stops the job
};

// Writes the dump of the field maps found on the disc (or the chosen ones) to `dir`.
// `cancelled`, when set, is polled between maps; the call then fails with the error "Cancelled.".
bool extract_disc(const std::filesystem::path& disc, const std::filesystem::path& dir, const std::vector<std::string>& only,
                  const std::function<void(const std::string&)>& log, std::string& error, const std::function<bool()>& cancelled = {});

// The realesrgan-ncnn-vulkan command line for one batch of images.
std::vector<std::string> upscaler_command(const PackJob& job, const std::filesystem::path& in_dir, const std::filesystem::path& out_dir);

bool run_pack_job(PackJob& job, GenerateReport& report, std::string& error);

}  // namespace dw3::texrep
