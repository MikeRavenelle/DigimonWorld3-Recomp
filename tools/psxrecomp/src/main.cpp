// psxrecomp: static recompiler, MIPS R3000A -> C.
// Pipeline (see docs/ARCHITECTURE.md): load config -> load exe + overlays -> decode ->
// function discovery -> per-function C emission -> function/overlay lookup tables.
//
// usage: psxrecomp <config.toml> analyze [--report <file>]
//        psxrecomp <config.toml> emit --out <dir> [--report <file>]

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <toml++/toml.hpp>

#include "psxdisc/psxexe.h"
#include "psxrecomp/analysis.h"
#include "psxrecomp/emitter.h"

namespace fs = std::filesystem;
using namespace psxrecomp;

namespace {

struct SegmentJob {
    CodeSegment seg;
    AnalysisConfig cfg;
};

std::vector<uint8_t> read_all(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// Expands "AAA/PRO/WSTAG*.PRO"-style patterns (single '*').
std::vector<std::string> expand_glob(const fs::path& root, const std::string& pattern) {
    std::vector<std::string> out;
    auto star = pattern.find('*');
    if (star == std::string::npos) return {pattern};
    fs::path dir = fs::path(pattern).parent_path();
    std::string fname = fs::path(pattern).filename().string();
    auto fstar = fname.find('*');
    std::string pre = fname.substr(0, fstar), post = fname.substr(fstar + 1);
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(root / dir, ec)) {
        std::string n = e.path().filename().string();
        if (n.size() >= pre.size() + post.size() && n.starts_with(pre) && n.ends_with(post))
            out.push_back((dir / n).generic_string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

void report_segment(FILE* out, const SegmentJob& job, const AnalysisResult& r, bool verbose) {
    std::size_t bios = 0, jt = 0, ijmp = 0, icall = 0, errs = 0;
    std::size_t by_src[6]{};
    uint64_t covered = 0;
    for (const auto& [s, f] : r.functions) {
        if (f.kind == FuncKind::BiosStub) ++bios;
        jt += f.jump_tables.size();
        ijmp += f.indirect_jumps.size();
        icall += f.indirect_calls.size();
        if (!f.errors.empty()) ++errs;
        covered += f.end - f.start;
        for (int b = 0; b < 6; ++b)
            if (f.source & (1u << b)) ++by_src[b];
    }
    uint32_t code_end = job.cfg.code_end ? job.cfg.code_end : job.seg.end();
    std::fprintf(out,
                 "%-14s base=%08X funcs=%4zu cover=%5.1f%% bios=%zu jtables=%zu unresolved_jr=%zu jalr=%zu "
                 "errfuncs=%zu warn=%zu ext=%zu  [entry=%zu jal=%zu tail=%zu ptr=%zu gap=%zu hint=%zu]\n",
                 job.seg.name.c_str(), job.seg.base, r.functions.size(),
                 100.0 * static_cast<double>(covered) / static_cast<double>(code_end - job.seg.base), bios, jt, ijmp,
                 icall, errs, r.warnings.size(), r.external_calls.size(), by_src[0], by_src[1], by_src[2], by_src[3],
                 by_src[4], by_src[5]);
    if (!verbose) return;
    for (const auto& [s, f] : r.functions) {
        std::fprintf(out, "  func %08X-%08X src=%02X%s", f.start, f.end, f.source,
                     f.kind == FuncKind::BiosStub ? " BIOS" : "");
        if (f.kind == FuncKind::BiosStub) std::fprintf(out, " %02X:%02X", f.bios_table, f.bios_fn);
        for (const auto& t : f.jump_tables) std::fprintf(out, " jt@%08X[%zu]", t.table_addr, t.targets.size());
        for (uint32_t a : f.indirect_jumps) std::fprintf(out, " jr?@%08X", a);
        for (const auto& e : f.errors) std::fprintf(out, " ERR(%s)", e.c_str());
        std::fprintf(out, "\n");
    }
    for (const auto& w : r.warnings) std::fprintf(out, "  WARN %s\n", w.c_str());
}

}  // namespace

bool write_if_changed(const fs::path& p, const std::string& text) {
    // Avoid touching unchanged files so incremental builds stay fast.
    {
        std::ifstream in(p, std::ios::binary);
        if (in) {
            std::string old{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
            if (old == text) return false;
        }
    }
    std::ofstream(p, std::ios::binary) << text;
    return true;
}

int main(int argc, char** argv) {
    std::string mode = argc >= 3 ? argv[2] : "";
    if (mode != "analyze" && mode != "emit") {
        std::fprintf(stderr, "usage: psxrecomp <config.toml> analyze [--report <file>]\n"
                             "       psxrecomp <config.toml> emit --out <dir> [--report <file>]\n");
        return 2;
    }
    std::string report_path, out_dir;
    for (int i = 3; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--report") report_path = argv[i + 1];
        if (std::string(argv[i]) == "--out") out_dir = argv[i + 1];
    }
    if (mode == "emit" && out_dir.empty()) {
        std::fprintf(stderr, "emit requires --out <dir>\n");
        return 2;
    }

    toml::table cfg;
    try {
        cfg = toml::parse_file(argv[1]);
    } catch (const toml::parse_error& e) {
        std::fprintf(stderr, "config error: %s\n", e.what());
        return 1;
    }
    fs::path extracted = cfg["paths"]["extracted"].value_or(std::string("game/extracted"));

    std::vector<SegmentJob> jobs;
    {
        auto file = read_all(extracted / cfg["exe"]["path"].value_or(std::string("SLUS_014.36")));
        auto exe = psxdisc::parse_psxexe(file);
        if (!exe) {
            std::fprintf(stderr, "cannot load exe from %s\n", extracted.string().c_str());
            return 1;
        }
        SegmentJob j;
        j.seg = {"MAIN", exe->text_addr, exe->text};
        j.cfg.entries.push_back(exe->pc);
        j.cfg.code_end = cfg["exe"]["code_end"].value_or(0u);
        if (auto* arr = cfg["exe"]["functions"].as_array())
            for (auto& v : *arr) j.cfg.extra_functions.insert(v.value_or(0u));
        jobs.push_back(std::move(j));
    }
    if (auto* slots = cfg["overlay_slot"].as_array()) {
        for (auto& node : *slots) {
            auto& slot = *node.as_table();
            uint32_t base = slot["base"].value_or(0u);
            std::vector<std::string> files;
            if (auto* a = slot["files"].as_array())
                for (auto& v : *a) files.push_back(v.value_or(std::string()));
            if (auto* a = slot["files_glob"].as_array())
                for (auto& v : *a)
                    for (auto& f : expand_glob(extracted, v.value_or(std::string()))) files.push_back(f);
            for (const auto& f : files) {
                SegmentJob j;
                j.seg = {fs::path(f).stem().string(), base, read_all(extracted / f)};
                if (j.seg.bytes.empty()) {
                    std::fprintf(stderr, "missing overlay %s\n", f.c_str());
                    return 1;
                }
                jobs.push_back(std::move(j));
            }
        }
    }

    FILE* rep = report_path.empty() ? nullptr : std::fopen(report_path.c_str(), "w");
    std::size_t total = 0;
    std::vector<AnalysisResult> results;
    for (const auto& job : jobs) {
        results.push_back(analyze(job.seg, job.cfg));
        total += results.back().functions.size();
        if (mode == "analyze") report_segment(stdout, job, results.back(), false);
        if (rep) report_segment(rep, job, results.back(), true);
    }
    std::printf("total functions: %zu across %zu segments\n", total, jobs.size());

    if (mode == "emit") {
        fs::create_directories(out_dir);
        std::vector<std::string> warnings;
        std::vector<TableSegment> tables;
        std::size_t files = 0, changed = 0;
        std::string manifest;
        for (std::size_t k = 0; k < jobs.size(); ++k) {
            EmitContext ec;
            ec.seg = &jobs[k].seg;
            ec.local = &results[k].functions;
            ec.main = &results[0].functions;  // job 0 is always MAIN
            for (auto& f : emit_segment(ec, 64, &warnings)) {
                changed += write_if_changed(fs::path(out_dir) / f.name, f.text);
                manifest += f.name + "\n";
                ++files;
            }
            uint32_t extent = jobs[k].cfg.code_end ? jobs[k].cfg.code_end - jobs[k].seg.base : 0;
            tables.push_back({&jobs[k].seg, &results[k].functions, extent});
        }
        auto t = emit_tables(tables, &warnings);
        changed += write_if_changed(fs::path(out_dir) / t.name, t.text);
        manifest += t.name + "\n";
        write_if_changed(fs::path(out_dir) / "sources.txt", manifest);
        std::printf("emitted %zu files (%zu changed) to %s, %zu warnings\n", files + 1, changed, out_dir.c_str(),
                    warnings.size());
        if (rep)
            for (const auto& w : warnings) std::fprintf(rep, "EMIT %s\n", w.c_str());
    }
    if (rep) std::fclose(rep);
    return 0;
}
