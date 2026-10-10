#pragma once

// Function discovery for a single code segment (main exe or one overlay).
//
// Discovery is reachability-based: the decoder accepts reserved-bit encodings, so a linear sweep
// would misread data as code. Seeds come from config entries, JAL targets found while tracing,
// pointers stored in the segment's data that land on function prologues, and gap filling
// between known functions (each candidate must trace cleanly to be accepted).

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "psxrecomp/mips.h"

namespace psxrecomp {

struct CodeSegment {
    std::string name;
    uint32_t base = 0;
    std::vector<uint8_t> bytes;

    uint32_t end() const { return base + static_cast<uint32_t>(bytes.size()); }
    bool contains(uint32_t addr) const { return addr >= base && addr < end(); }
    uint32_t word(uint32_t addr) const;  // 0 if out of range
};

struct JumpTable {
    uint32_t jr_addr = 0;     // address of the JR using the table
    uint32_t table_addr = 0;  // address of entry 0
    std::vector<uint32_t> targets;
};

enum class FuncKind : uint8_t {
    Normal,
    BiosStub,  // li t2,0xA0/B0/C0 ; jr t2 ; li t1,N
};

// Why a function start was accepted (bitmask).
enum FuncSource : uint32_t {
    kSourceEntry = 1u << 0,
    kSourceJal = 1u << 1,
    kSourceTailCall = 1u << 2,
    kSourceDataPointer = 1u << 3,
    kSourceGap = 1u << 4,
    kSourceHint = 1u << 5,
};

struct Function {
    uint32_t start = 0;
    uint32_t end = 0;  // exclusive; last instruction + 4 (includes final delay slot)
    FuncKind kind = FuncKind::Normal;
    uint32_t source = 0;
    uint8_t bios_table = 0;  // 0xA0/0xB0/0xC0 for BiosStub
    uint8_t bios_fn = 0;
    std::vector<uint32_t> insns;         // addresses of every reachable instruction (incl. delay slots), sorted
    std::set<uint32_t> callees;          // direct JAL/BAL targets (may be outside the segment)
    std::set<uint32_t> tail_calls;       // J/branch targets that are other functions
    std::vector<JumpTable> jump_tables;
    std::vector<uint32_t> indirect_calls;  // JALR sites
    std::vector<uint32_t> indirect_jumps;  // JR (not ra) sites with no recovered table
    std::vector<std::string> errors;       // decode failures, running off the segment, ...
};

struct AnalysisConfig {
    std::vector<uint32_t> entries;
    std::set<uint32_t> extra_functions;  // forced starts
    std::set<uint32_t> not_functions;    // rejected starts (false positives)
    uint32_t code_end = 0;               // gap fill / pointer scan limit for function starts (0 = segment end)
    bool scan_data_pointers = true;
    bool scan_jal_words = true;  // seed from any JAL word whose target is a prologue (overlays lack entries)
    bool gap_fill = true;
};

struct AnalysisResult {
    std::map<uint32_t, Function> functions;
    std::set<uint32_t> external_calls;  // direct call targets outside the segment
    std::vector<std::string> warnings;
};

AnalysisResult analyze(const CodeSegment& seg, const AnalysisConfig& cfg);

// Exposed for tests: true if `start` is `li t2,0xA0|0xB0|0xC0; jr t2; li t1,N`.
bool match_bios_stub(const CodeSegment& seg, uint32_t start, uint8_t& table, uint8_t& fn);

}  // namespace psxrecomp
