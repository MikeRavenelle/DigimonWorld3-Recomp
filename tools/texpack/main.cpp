// texpack: builds a texture pack from texture dumps and from the game's own files.
//
//   dw3recomp --dump-textures <dump dir> ...play a while...       records the background images the game draws
//   texpack generate --dump <dump dir> --out <pack dir>            upscales them with an external program
//   texpack generate --disc <disc image or folder> --out <pack dir>   the same for every field map on the disc
//   texpack extract --disc <disc image or folder> --out <dump dir>    only writes the dump of the disc's field maps
//
// The upscaler is realesrgan-ncnn-vulkan (https://github.com/xinntao/Real-ESRGAN-ncnn-vulkan); it and its model files
// are not shipped and are never downloaded by this tool (the first-run wizard of dw3recomp does that, on request).
// Point --upscaler and --models at an installation, or put them in <user data dir>/tools (realesrgan-ncnn-vulkan and
// models/).
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "dw3/platform.h"
#include "dw3/texrep/pack_job.h"

namespace fs = std::filesystem;
using namespace dw3::texrep;

namespace {

void usage() {
    std::fputs(
        "usage: texpack generate [--disc PATH] [--dump DIR ...] --out PACK_DIR [options]\n"
        "       texpack extract --disc PATH --out DUMP_DIR [--map S226 ...]\n"
        "\n"
        "  --disc PATH        the disc image (.cue or .bin) or an extracted copy of the disc: every field map on it is added\n"
        "  --map NAME         only these maps of the disc, such as S226 (repeatable)\n"
        "  --dump DIR         a directory written by dw3recomp --dump-textures (repeatable)\n"
        "  --out PACK_DIR     the pack to write; an existing pack of the same scale is extended\n"
        "  --name NAME        pack name stored in the manifest (default: the output folder name)\n"
        "  --scale N          replacement pixels per original texel: 2 or 4 (default 4)\n"
        "  --upscaler PATH    realesrgan-ncnn-vulkan executable (default: <user data dir>/tools)\n"
        "  --models DIR       folder with the model files (default: <user data dir>/tools/models)\n"
        "  --model NAME       model name without extension (default RealESRGAN_General_x4_v3)\n"
        "  --model-scale N    factor of the model (default 4)\n"
        "  --pad N            pixels of neighbouring picture given to the upscaler around each image (default 16)\n"
        "  --gpu N            Vulkan device for the upscaler (default: its own choice; use this if it picks a software device)\n"
        "  --work DIR         folder for temporary files (default: inside the system temporary folder)\n",
        stderr);
}

std::string quote(const std::string& s) {
#ifdef _WIN32
    std::string q = "\"";
    for (char c : s) {
        if (c == '"') q += "\\\"";
        else q += c;
    }
    return q + "\"";
#else
    std::string q = "'";
    for (char c : s) {
        if (c == '\'') q += "'\\''";
        else q += c;
    }
    return q + "'";
#endif
}

int run(const std::vector<std::string>& args) {
    std::string cmd;
    for (const std::string& a : args) cmd += (cmd.empty() ? "" : " ") + quote(a);
#ifdef _WIN32
    cmd = "\"" + cmd + "\"";  // cmd.exe strips one pair of quotes
#endif
    return std::system(cmd.c_str());
}

void log_line(const std::string& line) { std::fprintf(stderr, "texpack: %s\n", line.c_str()); }

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args = dw3::platform::command_line_utf8(argc, argv);
    if (args.size() < 2 || args[1] == "help" || args[1] == "--help" || args[1] == "-h") {
        usage();
        return args.size() < 2 ? 2 : 0;
    }
    if (args[1] != "generate" && args[1] != "extract") {
        usage();
        return 2;
    }
    const bool extract_only = args[1] == "extract";
    PackJob job;
    for (std::size_t i = 2; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto value = [&]() -> const std::string& {
            if (i + 1 >= args.size()) {
                std::fprintf(stderr, "texpack: %s needs a value\n", a.c_str());
                std::exit(2);
            }
            return args[++i];
        };
        if (a == "--dump") job.dumps.push_back(dw3::platform::path_from_utf8(value()));
        else if (a == "--disc") job.disc = dw3::platform::path_from_utf8(value());
        else if (a == "--map") job.maps.push_back(value());
        else if (a == "--out") job.out = dw3::platform::path_from_utf8(value());
        else if (a == "--name") job.name = value();
        else if (a == "--scale") job.scale = std::atoi(value().c_str());
        else if (a == "--upscaler") job.upscaler = dw3::platform::path_from_utf8(value());
        else if (a == "--models") job.models = dw3::platform::path_from_utf8(value());
        else if (a == "--model") job.model = value();
        else if (a == "--model-scale") job.model_scale = std::atoi(value().c_str());
        else if (a == "--pad") job.pad = std::atoi(value().c_str());
        else if (a == "--gpu") job.gpu = value();
        else if (a == "--work") job.work = dw3::platform::path_from_utf8(value());
        else {
            std::fprintf(stderr, "texpack: unknown option %s\n", a.c_str());
            usage();
            return 2;
        }
    }
    if (extract_only) {
        if (job.disc.empty() || job.out.empty()) {
            usage();
            return 2;
        }
        std::string error;
        if (!extract_disc(job.disc, job.out, job.maps, log_line, error)) {
            std::fprintf(stderr, "texpack: %s\n", error.c_str());
            return 1;
        }
        return 0;
    }
    if ((job.dumps.empty() && job.disc.empty()) || job.out.empty()) {
        usage();
        return 2;
    }
    const fs::path tools = dw3::platform::user_data_dir() / "tools";
#ifdef _WIN32
    const char* exe = "realesrgan-ncnn-vulkan.exe";
#else
    const char* exe = "realesrgan-ncnn-vulkan";
#endif
    if (job.upscaler.empty()) job.upscaler = tools / exe;
    if (job.models.empty()) job.models = tools / "models";
    if (!fs::exists(job.upscaler)) {
        std::fprintf(stderr, "texpack: upscaler not found: %s (see --upscaler)\n", dw3::platform::path_to_utf8(job.upscaler).c_str());
        return 1;
    }
    if (!fs::exists(job.models / (job.model + ".param"))) {
        std::fprintf(stderr, "texpack: model %s not found in %s (see --models, --model)\n", job.model.c_str(),
                     dw3::platform::path_to_utf8(job.models).c_str());
        return 1;
    }
    job.run = run;
    job.log = log_line;
    job.progress = [](std::size_t done, std::size_t total) { std::fprintf(stderr, "texpack: %zu / %zu\n", done, total); };

    GenerateReport report;
    std::string error;
    if (!run_pack_job(job, report, error)) {
        std::fprintf(stderr, "texpack: %s\n", error.c_str());
        return 1;
    }
    std::fprintf(stderr, "texpack: %zu images generated, %zu already in the pack -> %s\n", report.generated, report.skipped_existing,
                 dw3::platform::path_to_utf8(job.out).c_str());
    return 0;
}
