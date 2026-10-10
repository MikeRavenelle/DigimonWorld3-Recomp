#include "dw3/machine.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dw3 {

namespace {
FatalHandler g_fatal;

std::string hex(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof(b), "0x%08X", v);
    return b;
}

Machine* machine_of(RecompContext* ctx) {
    if (!ctx->machine) fatal("RecompContext has no Machine");
    return static_cast<Machine*>(ctx->machine);
}
}  // namespace

void set_fatal_handler(FatalHandler h) { g_fatal = std::move(h); }

void fatal(const std::string& message) {
    if (g_fatal) g_fatal(message);
    std::fprintf(stderr, "DW3 FATAL: %s\n", message.c_str());
    std::fflush(stderr);
    std::abort();
}

Machine::Machine(std::span<const RecompSegment> segments)
    : segments_(segments), resident_(segments.size(), false), verified_(segments.size(), false) {
    ctx().machine = this;
    ctx().next_event = INT64_MAX;
    if (!segments_.empty()) resident_[0] = verified_[0] = true;  // main executable
}

void Machine::load_segment_image(std::size_t index, std::span<const uint8_t> image) {
    const auto& s = segments_[index];
    std::memcpy(bus_.ram() + phys(s.base), image.data(), std::min<std::size_t>(image.size(), s.size));
    invalidate(s.base, s.size);
    for (std::size_t k = 0; k < segments_.size(); ++k)
        if (k != index && resident_[k] && covers(segments_[k], s.base)) resident_[k] = verified_[k] = false;
    resident_[index] = verified_[index] = true;
}

void Machine::invalidate(uint32_t addr, uint32_t len) {
    uint32_t lo = phys(addr), hi = lo + len;
    for (std::size_t k = 1; k < segments_.size(); ++k) {  // MAIN never moves
        uint32_t b = phys(segments_[k].base), e = b + segments_[k].size;
        if (lo < e && b < hi) verified_[k] = false;
    }
}

bool Machine::probes_match(const RecompSegment& s) const {
    if (s.probe_count == 0) return false;
    for (uint32_t i = 0; i < s.probe_count; ++i) {
        uint32_t w;
        std::memcpy(&w, bus_.ram() + phys(s.probes[i].addr), 4);
        if (w != s.probes[i].word) return false;
    }
    return true;
}

int Machine::resident_segment(uint32_t addr) {
    // Verified residents first (fast path).
    for (std::size_t k = 0; k < segments_.size(); ++k)
        if (verified_[k] && covers(segments_[k], addr)) return static_cast<int>(k);
    // Identify which candidate overlay is actually in RAM now.
    for (std::size_t k = 1; k < segments_.size(); ++k) {
        if (!covers(segments_[k], addr) || !probes_match(segments_[k])) continue;
        for (std::size_t j = 1; j < segments_.size(); ++j)
            if (j != k && resident_[j] && covers(segments_[j], segments_[k].base)) resident_[j] = verified_[j] = false;
        resident_[k] = verified_[k] = true;
        ++identifications_;
        return static_cast<int>(k);
    }
    return -1;
}

RecompFunc Machine::lookup(uint32_t addr) {
    ++lookups_;
    // Functions are keyed by their KSEG0 address; normalise KUSEG/KSEG1 aliases.
    uint32_t key = 0x80000000u | phys(addr);
    for (int pass = 0; pass < 2; ++pass) {
        int idx = resident_segment(key);
        if (idx >= 0) {
            const auto& s = segments_[static_cast<std::size_t>(idx)];
            auto* end = s.funcs + s.func_count;
            auto* it = std::lower_bound(s.funcs, end, key,
                                        [](const RecompFuncEntry& e, uint32_t a) { return e.addr < a; });
            if (it != end && it->addr == key) return it->func;
            // A stale "verified" overlay may hide the one actually loaded: re-identify once.
            if (pass == 0 && idx != 0) {
                verified_[static_cast<std::size_t>(idx)] = false;
                continue;
            }
            fatal("no recompiled function at " + hex(addr) + " in segment " + s.name);
        }
        break;
    }
    fatal("jump to " + hex(addr) + " outside any resident recompiled segment");
}

}  // namespace dw3

using dw3::fatal;

extern "C" {

RecompFunc dw3_lookup_func(RecompContext* ctx, uint32_t addr) { return dw3::machine_of(ctx)->lookup(addr); }

void dw3_service_events(RecompContext* ctx) {
    auto* m = dw3::machine_of(ctx);
    if (m->on_service_events) m->on_service_events(*ctx);
    else ctx->next_event = INT64_MAX;
}

void dw3_syscall(RecompContext* ctx, uint32_t pc) {
    auto* m = dw3::machine_of(ctx);
    if (!m->on_syscall) fatal("syscall at " + dw3::hex(pc) + " with no handler");
    m->on_syscall(*ctx, pc);
}

void dw3_break(RecompContext*, uint32_t pc) { fatal("BREAK at " + dw3::hex(pc)); }

void dw3_bad_jump(RecompContext*, uint32_t pc, uint32_t target) {
    fatal("jump table at " + dw3::hex(pc) + " went to unexpected " + dw3::hex(target));
}

void dw3_bios_call(RecompContext* ctx, uint32_t table, uint32_t fn) {
    auto* m = dw3::machine_of(ctx);
    if (!m->on_bios_call) {
        char b[64];
        std::snprintf(b, sizeof(b), "BIOS %02X:%02X with no handler", table, fn);
        fatal(b);
    }
    m->on_bios_call(*ctx, table, fn);
}

void dw3_mtc0(RecompContext* ctx, uint32_t reg, uint32_t value) { ctx->cop0[reg & 31] = value; }

void dw3_rfe(RecompContext* ctx) {
    // Pop the 3-level interrupt/mode stack in SR (bits 0-5).
    uint32_t sr = ctx->cop0[12];
    ctx->cop0[12] = (sr & ~0xFu) | ((sr >> 2) & 0xFu);
}

uint32_t dw3_gte_read_data(RecompContext* ctx, uint32_t reg) { return dw3::machine_of(ctx)->gte().read_data(reg); }
void dw3_gte_write_data(RecompContext* ctx, uint32_t reg, uint32_t value) {
    dw3::machine_of(ctx)->gte().write_data(reg, value);
}
uint32_t dw3_gte_read_ctrl(RecompContext* ctx, uint32_t reg) { return dw3::machine_of(ctx)->gte().read_ctrl(reg); }
void dw3_gte_write_ctrl(RecompContext* ctx, uint32_t reg, uint32_t value) {
    dw3::machine_of(ctx)->gte().write_ctrl(reg, value);
}
void dw3_gte_cmd(RecompContext* ctx, uint32_t cmd) {
    auto* m = dw3::machine_of(ctx);
    m->gte().execute(cmd);
    ctx->cycles += dw3::Gte::command_cycles(cmd);
}

}  // extern "C"
