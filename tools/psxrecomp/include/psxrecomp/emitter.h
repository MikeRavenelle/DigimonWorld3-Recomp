#pragma once

// C code emission for analysed functions. Generated code targets the contract in
// runtime/include/dw3/recomp.h.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "psxrecomp/analysis.h"

namespace psxrecomp {

struct EmitContext {
    const CodeSegment* seg = nullptr;
    const std::map<uint32_t, Function>* local = nullptr;  // functions of `seg`
    // Functions of the always-resident main executable, callable directly from overlays.
    // May equal `local` when emitting MAIN itself; may be null.
    const std::map<uint32_t, Function>* main = nullptr;
    std::string main_name = "MAIN";
    bool cycle_counting = true;
};

struct EmittedFile {
    std::string name;  // e.g. "MAIN_0.c"
    std::string text;
};

// "f_<SEG>_<ADDR>", e.g. f_MAIN_800144FC.
std::string func_name(const std::string& seg, uint32_t addr);

// C body of one function (definition only). Hazards and oddities are appended to `warnings`.
std::string emit_function(const EmitContext& ec, const Function& f, std::vector<std::string>* warnings = nullptr);

// Whole segment as one or more translation units (`funcs_per_file` functions each), with prototypes.
std::vector<EmittedFile> emit_segment(const EmitContext& ec, std::size_t funcs_per_file,
                                      std::vector<std::string>* warnings = nullptr);

struct TableSegment {
    const CodeSegment* seg;
    const std::map<uint32_t, Function>* funcs;
    // Address range this segment owns at runtime (0 = whole image). MAIN's image extends over the
    // overlay slots, so it only owns [base, code_end).
    uint32_t extent = 0;
};

// Lookup tables: one RecompFuncEntry array per segment plus `dw3_segments[]` / `dw3_segment_count`.
EmittedFile emit_tables(const std::vector<TableSegment>& segs);

// Code words used by the runtime to recognise which overlay occupies a slot.
std::vector<std::pair<uint32_t, uint32_t>> select_probes(const CodeSegment& seg,
                                                         const std::map<uint32_t, Function>& funcs,
                                                         std::size_t max_funcs = 8);

// FNV-1a over the segment image (used to identify which overlay is resident).
uint32_t segment_checksum(const CodeSegment& seg);

}  // namespace psxrecomp
