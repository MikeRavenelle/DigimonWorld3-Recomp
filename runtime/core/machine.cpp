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

// BIOS vectors reached by register jumps (jr/jalr to 0xA0/0xB0/0xC0); function number in $t1.
void bios_vector_a0(RecompContext* ctx) { dw3_bios_call(ctx, 0xA0, ctx->r[9]); }
void bios_vector_b0(RecompContext* ctx) { dw3_bios_call(ctx, 0xB0, ctx->r[9]); }
void bios_vector_c0(RecompContext* ctx) { dw3_bios_call(ctx, 0xC0, ctx->r[9]); }

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
    : sched_(bus_.ctx()),
      irq_(bus_),
      video_(sched_, irq_),
      timers_(bus_, sched_, irq_, video_),
      dma_(bus_, sched_, irq_),
      gpu_(bus_, irq_, video_, dma_),
      cdrom_(bus_, sched_, irq_, dma_),
      spu_(bus_, irq_, dma_),
      xa_(&cdrom_),
      sio_(bus_, sched_, irq_),
      mdec_(bus_, sched_, dma_),
      segments_(segments),
      resident_(segments.size(), false),
      verified_(segments.size(), false) {
    ctx().machine = this;
    gpu_.set_rasterizer(&soft_raster_);
    cdrom_.set_xa_sink(&xa_);
    spu_.set_cd_source(&xa_);
    // Memory/cache control: timing configuration with no functional effect here, but must read back.
    bus_.map_io({0x1F801000, 0x40, [this](uint32_t o, int) { return mem_ctrl_[o >> 2]; },
                 [this](uint32_t o, uint32_t v, int) { mem_ctrl_[o >> 2] = v; }});
    bus_.map_io({0x1F801060, 4, [this](uint32_t, int) { return ram_size_; },
                 [this](uint32_t, uint32_t v, int) { ram_size_ = v; }});
    bus_.map_io({0xFFFE0130, 4, [this](uint32_t, int) { return cache_ctrl_; },
                 [this](uint32_t, uint32_t v, int) { cache_ctrl_ = v; }});
    if (!segments_.empty()) resident_[0] = verified_[0] = true;  // main executable
    dma_.on_ram_written = [this](uint32_t addr, uint32_t len) { invalidate(addr, len); };
    irq_.on_line_change = [this](bool line) {
        set_cause_ip2(ctx(), line);
        if (line) request_service();
    };
}

void Machine::service_events() {
    sched_.run_due();
    if (on_interrupt && cpu_interrupt_deliverable(ctx(), irq_)) on_interrupt(ctx());
}

void Machine::enable_precise_geometry() {
    precise_.set_clock([this] { return video_.frame_count(); });
    gte_.set_precise_sink(&precise_);
    gpu_.set_precise_source(&precise_);
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

// Full check of an overlay's recompiled code against RAM. Many overlays share their first functions' words,
// so the probes alone can name the wrong one; this is only run when identifying, not on every lookup.
bool Machine::code_matches(const RecompSegment& s) const {
    if (s.range_count == 0) return true;
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < s.range_count; ++i) {
        const uint8_t* p = bus_.ram() + phys(s.ranges[i].start);
        for (uint32_t n = s.ranges[i].end - s.ranges[i].start; n; --n) h = (h ^ *p++) * 16777619u;
    }
    return h == s.code_hash;
}

int Machine::resident_segment(uint32_t addr) {
    // Verified residents first (fast path). The CPU may copy a different overlay over a resident one without
    // any DMA (the game stages stage code in the heap and copies it into place), so overlays are re-probed.
    for (std::size_t k = 0; k < segments_.size(); ++k) {
        if (!verified_[k] || !covers(segments_[k], addr)) continue;
        if (k == 0 || segments_[k].probe_count == 0 || probes_match(segments_[k])) return static_cast<int>(k);
        resident_[k] = verified_[k] = false;
    }
    // Identify which candidate overlay is actually in RAM now. When one overlay's code is a prefix of another's
    // both verify; the larger one wins.
    int best = -1;
    for (std::size_t k = 1; k < segments_.size(); ++k) {
        if (!covers(segments_[k], addr) || !probes_match(segments_[k]) || !code_matches(segments_[k])) continue;
        if (best < 0 || segments_[k].size > segments_[static_cast<std::size_t>(best)].size) best = static_cast<int>(k);
    }
    if (best < 0) return -1;
    auto k = static_cast<std::size_t>(best);
    for (std::size_t j = 1; j < segments_.size(); ++j)
        if (j != k && resident_[j] && covers(segments_[j], segments_[k].base)) resident_[j] = verified_[j] = false;
    resident_[k] = verified_[k] = true;
    ++identifications_;
    return best;
}

RecompFunc Machine::lookup(uint32_t addr) {
    ++lookups_;
    // Functions are keyed by their KSEG0 address; normalise KUSEG/KSEG1 aliases.
    uint32_t key = 0x80000000u | phys(addr);
    switch (phys(addr)) {
        case 0xA0: return bios_vector_a0;
        case 0xB0: return bios_vector_b0;
        case 0xC0: return bios_vector_c0;
        default: break;
    }
    if (phys(addr) < 0x10000 && !natives_.empty()) {
        auto it = natives_.find(phys(addr));
        if (it != natives_.end()) return it->second;
    }
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
            fatal("no recompiled function at " + hex(addr) + " in segment " + s.name + " (ra " + hex(ctx().r[31]) + ")");
        }
        break;
    }
    fatal("jump to " + hex(addr) + " outside any resident recompiled segment (ra " + hex(ctx().r[31]) + ", sp " + hex(ctx().r[29]) + ")");
}

}  // namespace dw3

using dw3::fatal;

extern "C" {

RecompFunc dw3_lookup_func(RecompContext* ctx, uint32_t addr) { return dw3::machine_of(ctx)->lookup(addr); }

void dw3_service_events(RecompContext* ctx) {
    auto* m = dw3::machine_of(ctx);
    if (m->on_service_events) m->on_service_events(*ctx);
    else m->service_events();
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

void dw3_mtc0(RecompContext* ctx, uint32_t reg, uint32_t value) {
    ctx->cop0[reg & 31] = value;
    // Enabling interrupts with the line already high must deliver promptly.
    if ((reg & 31) == 12 && ctx->machine) dw3::machine_of(ctx)->request_service();
}

void dw3_rfe(RecompContext* ctx) {
    // Pop the 3-level interrupt/mode stack in SR (bits 0-5).
    uint32_t sr = ctx->cop0[12];
    ctx->cop0[12] = (sr & ~0xFu) | ((sr >> 2) & 0xFu);
    if (ctx->machine) dw3::machine_of(ctx)->request_service();
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
