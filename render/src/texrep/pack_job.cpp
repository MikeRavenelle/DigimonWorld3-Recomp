#include "dw3/texrep/pack_job.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>

#include "dw3/platform.h"
#include "dw3/texrep/pack.h"
#include "dw3/texrep/stage_dump.h"

namespace dw3::texrep {

namespace fs = std::filesystem;

bool extract_disc(const fs::path& disc, const fs::path& dir, const std::vector<std::string>& only,
                  const std::function<void(const std::string&)>& log, std::string& error, const std::function<bool()>& cancelled) {
    std::unique_ptr<StageSource> source = StageSource::open(disc, error);
    if (!source) return false;
    StageDumpWriter writer(dir);
    std::vector<uint8_t> data;
    std::size_t index = 0;
    for (const std::string& map : source->maps()) {
        ++index;
        if (cancelled && cancelled()) {
            error = "Cancelled.";
            return false;
        }
        if (!only.empty() && std::find(only.begin(), only.end(), map) == only.end()) continue;
        StagePack pack;
        std::string detail;
        if (!source->read(map, data, detail) || !parse_stage_pack(data, pack, detail)) {
            error = map + ": " + detail;
            return false;
        }
        if (!writer.add_map(map, pack, error)) return false;
        if (log && index % 20 == 0) log("read " + std::to_string(index) + " / " + std::to_string(source->maps().size()) + " maps");
    }
    if (!writer.finish(error)) return false;
    const StageDumpStats& st = writer.stats();
    if (log)
        log(std::to_string(st.maps) + " maps, " + std::to_string(st.cells) + " pictures, " + std::to_string(st.images) + " distinct images (" +
            std::to_string(st.untracked) + " not trackable, " + std::to_string(st.transparent) + " empty) -> " + platform::path_to_utf8(dir));
    return true;
}

std::vector<std::string> upscaler_command(const PackJob& job, const fs::path& in_dir, const fs::path& out_dir) {
    std::vector<std::string> cmd = {platform::path_to_utf8(job.upscaler), "-i", platform::path_to_utf8(in_dir), "-o",
                                    platform::path_to_utf8(out_dir), "-n", job.model, "-s", std::to_string(job.model_scale), "-m",
                                    platform::path_to_utf8(job.models), "-f", "png"};
    if (!job.gpu.empty()) {
        cmd.push_back("-g");
        cmd.push_back(job.gpu);
    }
    return cmd;
}

bool run_pack_job(PackJob& job, GenerateReport& report, std::string& error) {
    std::error_code ec;
    const bool own_work = job.work.empty();
    if (own_work) job.work = fs::temp_directory_path(ec) / ("dw3texpack-" + std::to_string(std::rand()));
    fs::create_directories(job.work / "in", ec);
    auto finish = [&](bool ok) {
        if (own_work) fs::remove_all(job.work, ec);
        return ok;
    };
    std::vector<fs::path> dumps = job.dumps;
    if (!job.disc.empty()) {
        if (!extract_disc(job.disc, job.work / "disc-dump", job.maps, job.log, error, job.cancelled)) return finish(false);
        dumps.insert(dumps.begin(), job.work / "disc-dump");
    }

    GenerateOptions options;
    options.name = job.name.empty() ? platform::path_to_utf8(job.out.filename()) : job.name;
    options.scale = job.scale;
    options.model_scale = job.model_scale;
    options.pad = job.pad;
    options.progress = job.progress;

    BatchUpscaler upscale = [&](const std::vector<Image>& in, std::vector<Image>& out, std::string& err) {
        fs::remove_all(job.work / "in", ec);
        fs::remove_all(job.work / "out", ec);
        fs::create_directories(job.work / "in", ec);
        fs::create_directories(job.work / "out", ec);
        char name[32];
        for (std::size_t i = 0; i < in.size(); ++i) {
            if (job.cancelled && job.cancelled()) {
                err = "Cancelled.";
                return false;
            }
            std::snprintf(name, sizeof name, "%05zu.png", i);
            if (!write_png(job.work / "in" / name, in[i], err, false)) return false;  // the upscaler mishandles alpha
        }
        const std::vector<std::string> cmd = upscaler_command(job, job.work / "in", job.work / "out");
        const int rc = job.run(cmd);
        if (rc != 0) {
            err = rc < 0 ? "the upscaler was stopped" : "the upscaler failed (exit code " + std::to_string(rc) + ")";
            return false;
        }
        out.clear();
        for (std::size_t i = 0; i < in.size(); ++i) {
            std::snprintf(name, sizeof name, "%05zu.png", i);
            Image img;
            if (!read_png(job.work / "out" / name, img, err)) return false;
            out.push_back(std::move(img));
        }
        return true;
    };
    return finish(generate_pack(dumps, job.out, options, upscale, report, error));
}

}  // namespace dw3::texrep
