#include "ref_interp.h"

#include <cstdio>

namespace refint {

namespace {

struct Pending {
    bool active = false;
    unsigned reg = 0;
    uint32_t val = 0;
};

}  // namespace

Result run(RecompContext& ctx, dw3::Bus& bus, uint32_t entry, uint64_t max_steps) {
    Result res;
    uint32_t pc = entry, npc = entry + 4;
    Pending pend;
    uint32_t* r = ctx.r;

    auto fault = [&](const char* why, uint32_t at, uint32_t w) {
        char b[96];
        std::snprintf(b, sizeof(b), "%s at %08X (word %08X)", why, at, w);
        res.faulted = true;
        res.error = b;
    };
    auto rd8 = [&](uint32_t a) { return static_cast<uint32_t>(bus.read(a, 1)) & 0xFFu; };

    while (res.steps < max_steps) {
        if (pc == kReturnSentinel) {
            res.returned = true;
            break;
        }
        const uint32_t cur = pc;
        const uint32_t w = bus.read(cur, 4);
        pc = npc;
        npc = pc + 4;
        ++res.steps;

        const uint32_t op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31;
        const uint32_t sh = (w >> 6) & 31, fn = w & 63;
        const uint32_t uimm = w & 0xFFFF;
        const int32_t simm = static_cast<int16_t>(w & 0xFFFF);
        const int32_t sa = static_cast<int32_t>(r[rs]), sb = static_cast<int32_t>(r[rt]);

        int alu_dest = -1;  // register written by a non-load instruction this step
        Pending np;
        auto wr = [&](unsigned reg, uint32_t v) {
            alu_dest = static_cast<int>(reg);
            if (reg) r[reg] = v;
        };
        auto ld = [&](unsigned reg, uint32_t v) {
            if (reg) np = {true, reg, v};
        };
        auto branch = [&](bool taken) {
            if (taken) npc = cur + 4 + static_cast<uint32_t>(simm * 4);
        };
        const uint32_t ea = r[rs] + static_cast<uint32_t>(simm);
        // Value of $rt as seen by LWL/LWR: the in-flight load result is forwarded to them.
        const uint32_t rt_fwd = (pend.active && pend.reg == rt) ? pend.val : r[rt];

        switch (op) {
            case 0x00:
                switch (fn) {
                    case 0x00: wr(rd, r[rt] << sh); break;
                    case 0x02: wr(rd, r[rt] >> sh); break;
                    case 0x03: wr(rd, static_cast<uint32_t>(sb >> sh)); break;
                    case 0x04: wr(rd, r[rt] << (r[rs] & 31)); break;
                    case 0x06: wr(rd, r[rt] >> (r[rs] & 31)); break;
                    case 0x07: wr(rd, static_cast<uint32_t>(sb >> (r[rs] & 31))); break;
                    case 0x08: npc = r[rs]; break;
                    case 0x09: {
                        uint32_t t = r[rs];
                        wr(rd, cur + 8);
                        npc = t;
                        break;
                    }
                    case 0x10: wr(rd, ctx.hi); break;
                    case 0x11: ctx.hi = r[rs]; break;
                    case 0x12: wr(rd, ctx.lo); break;
                    case 0x13: ctx.lo = r[rs]; break;
                    case 0x18: {
                        int64_t p = static_cast<int64_t>(sa) * static_cast<int64_t>(sb);
                        ctx.lo = static_cast<uint32_t>(static_cast<uint64_t>(p));
                        ctx.hi = static_cast<uint32_t>(static_cast<uint64_t>(p) >> 32);
                        break;
                    }
                    case 0x19: {
                        uint64_t p = static_cast<uint64_t>(r[rs]) * static_cast<uint64_t>(r[rt]);
                        ctx.lo = static_cast<uint32_t>(p);
                        ctx.hi = static_cast<uint32_t>(p >> 32);
                        break;
                    }
                    case 0x1A:
                        if (sb == 0) {
                            ctx.lo = sa < 0 ? 1u : 0xFFFFFFFFu;
                            ctx.hi = r[rs];
                        } else {
                            int64_t q = static_cast<int64_t>(sa) / sb, m = static_cast<int64_t>(sa) % sb;
                            ctx.lo = static_cast<uint32_t>(static_cast<uint64_t>(q));  // INT_MIN/-1 wraps to INT_MIN
                            ctx.hi = static_cast<uint32_t>(static_cast<uint64_t>(m));
                        }
                        break;
                    case 0x1B:
                        if (r[rt] == 0) {
                            ctx.lo = 0xFFFFFFFFu;
                            ctx.hi = r[rs];
                        } else {
                            ctx.lo = r[rs] / r[rt];
                            ctx.hi = r[rs] % r[rt];
                        }
                        break;
                    case 0x20: case 0x21: wr(rd, r[rs] + r[rt]); break;  // ADD traps are not modelled
                    case 0x22: case 0x23: wr(rd, r[rs] - r[rt]); break;
                    case 0x24: wr(rd, r[rs] & r[rt]); break;
                    case 0x25: wr(rd, r[rs] | r[rt]); break;
                    case 0x26: wr(rd, r[rs] ^ r[rt]); break;
                    case 0x27: wr(rd, ~(r[rs] | r[rt])); break;
                    case 0x2A: wr(rd, sa < sb ? 1u : 0u); break;
                    case 0x2B: wr(rd, r[rs] < r[rt] ? 1u : 0u); break;
                    default: fault("unsupported SPECIAL", cur, w); break;
                }
                break;
            case 0x01: {
                const bool lt = sa < 0;
                switch (rt) {
                    case 0x00: branch(lt); break;
                    case 0x01: branch(!lt); break;
                    case 0x10: wr(31, cur + 8); branch(lt); break;
                    case 0x11: wr(31, cur + 8); branch(!lt); break;
                    default: fault("unsupported REGIMM", cur, w); break;
                }
                break;
            }
            case 0x02: npc = ((cur + 4) & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2); break;
            case 0x03:
                wr(31, cur + 8);
                npc = ((cur + 4) & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2);
                break;
            case 0x04: branch(r[rs] == r[rt]); break;
            case 0x05: branch(r[rs] != r[rt]); break;
            case 0x06: branch(sa <= 0); break;
            case 0x07: branch(sa > 0); break;
            case 0x08: case 0x09: wr(rt, r[rs] + static_cast<uint32_t>(simm)); break;
            case 0x0A: wr(rt, sa < simm ? 1u : 0u); break;
            case 0x0B: wr(rt, r[rs] < static_cast<uint32_t>(simm) ? 1u : 0u); break;
            case 0x0C: wr(rt, r[rs] & uimm); break;
            case 0x0D: wr(rt, r[rs] | uimm); break;
            case 0x0E: wr(rt, r[rs] ^ uimm); break;
            case 0x0F: wr(rt, uimm << 16); break;
            case 0x20: ld(rt, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(rd8(ea))))); break;
            case 0x24: ld(rt, rd8(ea)); break;
            case 0x21:
            case 0x25: {
                if (ea & 1) { fault("unaligned halfword load", cur, w); break; }
                uint32_t v = rd8(ea) | (rd8(ea + 1) << 8);
                ld(rt, op == 0x21 ? static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(v))) : v);
                break;
            }
            case 0x23: {
                if (ea & 3) { fault("unaligned word load", cur, w); break; }
                ld(rt, rd8(ea) | (rd8(ea + 1) << 8) | (rd8(ea + 2) << 16) | (rd8(ea + 3) << 24));
                break;
            }
            case 0x22: {  // LWL: bytes ea, ea-1, ... down to the word boundary fill the top of rt
                uint32_t v = rt_fwd;
                for (uint32_t k = 0; k <= (ea & 3); ++k) {
                    uint32_t shift = 8 * (3 - k);
                    v = (v & ~(0xFFu << shift)) | (rd8(ea - k) << shift);
                }
                ld(rt, v);
                break;
            }
            case 0x26: {  // LWR: bytes ea .. word end fill the bottom of rt
                uint32_t v = rt_fwd;
                for (uint32_t k = 0; k <= 3 - (ea & 3); ++k) {
                    uint32_t shift = 8 * k;
                    v = (v & ~(0xFFu << shift)) | (rd8(ea + k) << shift);
                }
                ld(rt, v);
                break;
            }
            case 0x28: bus.write(ea, r[rt] & 0xFFu, 1); break;
            case 0x29:
                if (ea & 1) { fault("unaligned halfword store", cur, w); break; }
                bus.write(ea, r[rt] & 0xFFu, 1);
                bus.write(ea + 1, (r[rt] >> 8) & 0xFFu, 1);
                break;
            case 0x2B:
                if (ea & 3) { fault("unaligned word store", cur, w); break; }
                for (uint32_t k = 0; k < 4; ++k) bus.write(ea + k, (r[rt] >> (8 * k)) & 0xFFu, 1);
                break;
            case 0x2A:
                for (uint32_t k = 0; k <= (ea & 3); ++k) bus.write(ea - k, (r[rt] >> (8 * (3 - k))) & 0xFFu, 1);
                break;
            case 0x2E:
                for (uint32_t k = 0; k <= 3 - (ea & 3); ++k) bus.write(ea + k, (r[rt] >> (8 * k)) & 0xFFu, 1);
                break;
            default: fault("unsupported opcode", cur, w); break;
        }
        if (res.faulted) {
            res.pc = cur;
            return res;
        }
        // The load issued by the previous instruction lands now unless this one overwrote its target.
        if (pend.active && alu_dest != static_cast<int>(pend.reg)) r[pend.reg] = pend.val;
        pend = np;
        r[0] = 0;
    }
    if (pend.active) r[pend.reg] = pend.val;  // a load still in flight when we stop
    r[0] = 0;
    res.pc = pc;
    return res;
}

}  // namespace refint
