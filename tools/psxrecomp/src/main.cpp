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
#include "psxrecomp/pipeline.h"

namespace fs = std::filesystem;
using namespace psxrecomp;

namespace {

void report_segment(FILE* out, const SegmentJob& job, const AnalysisResult& r, bool verbose) {
    std::size_t bios = 0, jt = 0, pjmp = 0, ijmp = 0, icall = 0, errs = 0;
    std::size_t by_src[6]{};
    uint64_t covered = 0;
    for (const auto& [s, f] : r.functions) {
        if (f.kind == FuncKind::BiosStub) ++bios;
        jt += f.jump_tables.size();
        pjmp += f.pointer_jumps.size();
        ijmp += f.indirect_jumps.size();
        icall += f.indirect_calls.size();
        if (!f.errors.empty()) ++errs;
        covered += f.end - f.start;
        for (int b = 0; b < 6; ++b)
            if (f.source & (1u << b)) ++by_src[b];
    }
    uint32_t code_end = job.cfg.code_end ? job.cfg.code_end : job.seg.end();
    std::fprintf(out,
                 "%-14s base=%08X funcs=%4zu cover=%5.1f%% bios=%zu jtables=%zu ptr_jr=%zu unresolved_jr=%zu jalr=%zu "
                 "errfuncs=%zu warn=%zu ext=%zu  [entry=%zu jal=%zu tail=%zu ptr=%zu gap=%zu hint=%zu]\n",
                 job.seg.name.c_str(), job.seg.base, r.functions.size(),
                 100.0 * static_cast<double>(covered) / static_cast<double>(code_end - job.seg.base), bios, jt, pjmp, ijmp,
                 icall, errs, r.warnings.size(), r.external_calls.size(), by_src[0], by_src[1], by_src[2], by_src[3],
                 by_src[4], by_src[5]);
    if (!verbose) return;
    for (const auto& [s, f] : r.functions) {
        std::fprintf(out, "  func %08X-%08X src=%02X%s", f.start, f.end, f.source,
                     f.kind == FuncKind::BiosStub ? " BIOS" : "");
        if (f.kind == FuncKind::BiosStub) std::fprintf(out, " %02X:%02X", f.bios_table, f.bios_fn);
        for (const auto& t : f.jump_tables) std::fprintf(out, " jt@%08X[%zu]", t.table_addr, t.targets.size());
        for (const auto& pj : f.pointer_jumps)
            std::fprintf(out, " %s@%08X->%08X", pj.via_slot ? "jr[slot]" : "jr[const]", pj.jr_addr, pj.addr);
        for (uint32_t a : f.indirect_jumps) std::fprintf(out, " jr?@%08X", a);
        for (const auto& e : f.errors) std::fprintf(out, " ERR(%s)", e.c_str());
        std::fprintf(out, "\n");
    }
    for (const auto& w : r.warnings) std::fprintf(out, "  WARN %s\n", w.c_str());
}

}  // namespace

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

    Game game;
    std::string error;
    if (!game.load(cfg, directory_source(extracted), error)) {
        std::fprintf(stderr, "%s (in %s)\n", error.c_str(), extracted.string().c_str());
        return 1;
    }
    game.analyze({});
    const auto& jobs = game.jobs();
    const auto& results = game.results();

    FILE* rep = report_path.empty() ? nullptr : std::fopen(report_path.c_str(), "w");
    std::size_t total = 0;
    for (std::size_t k = 0; k < jobs.size(); ++k) {
        total += results[k].functions.size();
        if (mode == "analyze") report_segment(stdout, jobs[k], results[k], false);
        if (rep) report_segment(rep, jobs[k], results[k], true);
    }
    std::printf("total functions: %zu across %zu segments\n", total, jobs.size());

    EmitStats st = game.statistics();
    for (const auto& a : st.ambiguous_overlays) std::printf("ambiguous overlays: %s\n", a.c_str());
    std::printf("summary: %zu functions, %zu jump tables, %zu pointer jumps, %zu unresolved jr, %zu functions with errors, "
                "%zu overlays with stale-tail checks (%zu links), %zu ambiguous overlay pairs\n",
                st.functions, st.jump_tables, st.pointer_jumps, st.unresolved_jumps, st.functions_with_errors,
                st.stale_overlays, st.stale_links, st.ambiguous_overlays.size());

    if (mode == "emit") {
        EmitStats emitted;
        if (!game.emit(out_dir, {}, emitted, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        std::printf("emitted %zu files (%zu changed) to %s, %zu warnings\n", emitted.files, emitted.files_changed,
                    out_dir.c_str(), emitted.warnings.size());
        if (rep)
            for (const auto& w : emitted.warnings) std::fprintf(rep, "EMIT %s\n", w.c_str());
    }
    if (rep) std::fclose(rep);
    return 0;
}
