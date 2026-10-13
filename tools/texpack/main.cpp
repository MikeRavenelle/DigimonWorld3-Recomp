// texpack: builds a texture pack from a texture dump.
//
//   dw3recomp --dump-textures <dump dir> ...play a while...       records the background images the game draws
//   texpack generate --dump <dump dir> --out <pack dir>            upscales them with an external program
//
// The upscaler is realesrgan-ncnn-vulkan (https://github.com/xinntao/Real-ESRGAN-ncnn-vulkan); it and its model files
// are not shipped and are never downloaded by this tool. Point --upscaler and --models at an installation, or put them
// in <user data dir>/tools (realesrgan-ncnn-vulkan and models/).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "dw3/platform.h"
#include "dw3/texrep/generate.h"

namespace fs = std::filesystem;
using namespace dw3::texrep;

namespace {

void usage() {
    std::fputs(
        "usage: texpack generate --dump DIR [--dump DIR ...] --out PACK_DIR [options]\n"
        "\n"
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

struct Config {
    std::vector<fs::path> dumps;
    fs::path out, upscaler, models, work;
    std::string name, model = "RealESRGAN_General_x4_v3", gpu;
    int scale = 4, model_scale = 4, pad = 16;
};

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args = dw3::platform::command_line_utf8(argc, argv);
    if (args.size() < 2 || args[1] == "help" || args[1] == "--help" || args[1] == "-h") {
        usage();
        return args.size() < 2 ? 2 : 0;
    }
    if (args[1] != "generate") {
        usage();
        return 2;
    }
    Config c;
    for (std::size_t i = 2; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto value = [&]() -> const std::string& {
            if (i + 1 >= args.size()) {
                std::fprintf(stderr, "texpack: %s needs a value\n", a.c_str());
                std::exit(2);
            }
            return args[++i];
        };
        if (a == "--dump") c.dumps.push_back(dw3::platform::path_from_utf8(value()));
        else if (a == "--out") c.out = dw3::platform::path_from_utf8(value());
        else if (a == "--name") c.name = value();
        else if (a == "--scale") c.scale = std::atoi(value().c_str());
        else if (a == "--upscaler") c.upscaler = dw3::platform::path_from_utf8(value());
        else if (a == "--models") c.models = dw3::platform::path_from_utf8(value());
        else if (a == "--model") c.model = value();
        else if (a == "--model-scale") c.model_scale = std::atoi(value().c_str());
        else if (a == "--pad") c.pad = std::atoi(value().c_str());
        else if (a == "--gpu") c.gpu = value();
        else if (a == "--work") c.work = dw3::platform::path_from_utf8(value());
        else {
            std::fprintf(stderr, "texpack: unknown option %s\n", a.c_str());
            usage();
            return 2;
        }
    }
    if (c.dumps.empty() || c.out.empty()) {
        usage();
        return 2;
    }
    const fs::path tools = dw3::platform::user_data_dir() / "tools";
#ifdef _WIN32
    const char* exe = "realesrgan-ncnn-vulkan.exe";
#else
    const char* exe = "realesrgan-ncnn-vulkan";
#endif
    if (c.upscaler.empty()) c.upscaler = tools / exe;
    if (c.models.empty()) c.models = tools / "models";
    if (!fs::exists(c.upscaler)) {
        std::fprintf(stderr, "texpack: upscaler not found: %s (see --upscaler)\n", dw3::platform::path_to_utf8(c.upscaler).c_str());
        return 1;
    }
    if (!fs::exists(c.models / (c.model + ".param"))) {
        std::fprintf(stderr, "texpack: model %s not found in %s (see --models, --model)\n", c.model.c_str(),
                     dw3::platform::path_to_utf8(c.models).c_str());
        return 1;
    }
    std::error_code ec;
    const bool own_work = c.work.empty();
    if (own_work) c.work = fs::temp_directory_path(ec) / ("dw3texpack-" + std::to_string(std::rand()));
    fs::create_directories(c.work / "in", ec);

    GenerateOptions options;
    options.name = c.name.empty() ? dw3::platform::path_to_utf8(c.out.filename()) : c.name;
    options.scale = c.scale;
    options.model_scale = c.model_scale;
    options.pad = c.pad;
    options.progress = [](std::size_t done, std::size_t total) { std::fprintf(stderr, "texpack: %zu / %zu\n", done, total); };

    BatchUpscaler upscale = [&](const std::vector<Image>& in, std::vector<Image>& out, std::string& error) {
        fs::remove_all(c.work / "in", ec);
        fs::remove_all(c.work / "out", ec);
        fs::create_directories(c.work / "in", ec);
        fs::create_directories(c.work / "out", ec);
        char name[32];
        for (std::size_t i = 0; i < in.size(); ++i) {
            std::snprintf(name, sizeof name, "%05zu.png", i);
            if (!write_png(c.work / "in" / name, in[i], error, false)) return false;  // the upscaler mishandles alpha
        }
        std::vector<std::string> cmd = {dw3::platform::path_to_utf8(c.upscaler), "-i", dw3::platform::path_to_utf8(c.work / "in"), "-o",
                                        dw3::platform::path_to_utf8(c.work / "out"), "-n", c.model, "-s", std::to_string(c.model_scale), "-m",
                                        dw3::platform::path_to_utf8(c.models), "-f", "png"};
        if (!c.gpu.empty()) {
            cmd.push_back("-g");
            cmd.push_back(c.gpu);
        }
        const int rc = run(cmd);
        if (rc != 0) {
            error = "the upscaler failed (exit code " + std::to_string(rc) + ")";
            return false;
        }
        out.clear();
        for (std::size_t i = 0; i < in.size(); ++i) {
            std::snprintf(name, sizeof name, "%05zu.png", i);
            Image img;
            if (!read_png(c.work / "out" / name, img, error)) return false;
            out.push_back(std::move(img));
        }
        return true;
    };

    GenerateReport report;
    std::string error;
    const bool ok = generate_pack(c.dumps, c.out, options, upscale, report, error);
    if (own_work) fs::remove_all(c.work, ec);
    if (!ok) {
        std::fprintf(stderr, "texpack: %s\n", error.c_str());
        return 1;
    }
    std::fprintf(stderr, "texpack: %zu images generated, %zu already in the pack -> %s\n", report.generated,
                 report.skipped_existing, dw3::platform::path_to_utf8(c.out).c_str());
    return 0;
}
