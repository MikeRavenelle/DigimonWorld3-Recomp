// Native PlayStation kernel. Function numbers and semantics follow psx-spx "BIOS Function Summary",
// "BIOS Event Functions" and "BIOS Interrupt/Exception Handling".

#include "dw3/bios.h"

#include <cstdio>
#include <cstring>

namespace dw3 {

namespace {

constexpr uint32_t kGameReturnSentinel = 0x00000000;  // $ra for kernel-initiated calls into game code

// Event classes/specs (psx-spx).
constexpr uint32_t kClassRCnt = 0xF2000000;
constexpr uint32_t kClassHwCard = 0xF0000011;
constexpr uint32_t kClassSwCard = 0xF4000001;
constexpr uint32_t kSpecInt = 0x0002;
constexpr uint32_t kSpecTimeout = 0x0100;

constexpr uint32_t kIStat = 0x1F801070;
constexpr uint32_t kIMask = 0x1F801074;
constexpr uint32_t kGp0 = 0x1F801810;

constexpr uint32_t kSrIEc = 1u << 0;
constexpr uint32_t kSrIM2 = 1u << 10;

std::string hexs(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof(b), "%08X", v);
    return b;
}

// Saved CPU state around kernel work that runs game code.
struct SavedRegs {
    uint32_t r[32], hi, lo;
    explicit SavedRegs(const RecompContext& c) : hi(c.hi), lo(c.lo) { std::memcpy(r, c.r, sizeof(r)); }
    void restore(RecompContext& c) const {
        std::memcpy(c.r, r, sizeof(r));
        c.hi = hi;
        c.lo = lo;
    }
};

}  // namespace

Bios::Bios(Machine& m) : m_(m) {
    m_.on_bios_call = [this](RecompContext& c, uint32_t t, uint32_t f) { call(c, t, f); };
    m_.on_syscall = [this](RecompContext& c, uint32_t pc) { syscall(c, pc); };
    m_.on_interrupt = [this](RecompContext& c) { interrupt(c); };
}

uint32_t Bios::load_exe(std::span<const uint8_t> f, uint32_t stack) {
    auto u32 = [&](std::size_t o) {
        uint32_t v;
        std::memcpy(&v, f.data() + o, 4);
        return v;
    };
    if (f.size() < 0x800 || std::memcmp(f.data(), "PS-X EXE", 8) != 0) fatal("load_exe: not a PS-X EXE");
    uint32_t pc = u32(0x10), gp = u32(0x14), addr = u32(0x18), size = u32(0x1C);
    uint32_t bss = u32(0x28), bss_size = u32(0x2C), sp_base = u32(0x30), sp_off = u32(0x34);
    if (size > f.size() - 0x800) fatal("load_exe: truncated text");
    uint64_t text_end = uint64_t{dw3_phys(addr) & 0x1FFFFF} + size;
    uint64_t bss_end = uint64_t{dw3_phys(bss) & 0x1FFFFF} + bss_size;
    if (text_end > DW3_RAM_SIZE || bss_end > DW3_RAM_SIZE) fatal("load_exe: image exceeds RAM");
    RecompContext& c = m_.ctx();
    uint8_t* ram = m_.bus().ram();
    std::memcpy(ram + (dw3_phys(addr) & 0x1FFFFF), f.data() + 0x800, size);
    if (bss_size) std::memset(ram + (dw3_phys(bss) & 0x1FFFFF), 0, bss_size);
    m_.invalidate(addr, size);
    std::memset(c.r, 0, sizeof(c.r));
    c.r[28] = gp;
    c.r[29] = c.r[30] = sp_base ? sp_base + sp_off : stack;
    c.cop0[12] = 0x40000000;  // COP2 usable, interrupts disabled until the game enables them
    // Post-boot hardware state the real BIOS leaves behind: CD-ROM interrupts enabled (libcd's first
    // command relies on it before CD_init programs the enable register itself).
    m_.bus().write(0x1F801800, 1, 1);
    m_.bus().write(0x1F801802, 0x1F, 1);
    m_.bus().write(0x1F801800, 0, 1);
    return pc;
}

void Bios::run(uint32_t entry) {
    RecompContext& c = m_.ctx();
    c.r[31] = kGameReturnSentinel;
    dw3_call(&c, entry);
}

// ---------------------------------------------------------------------------------------------
// Events

uint32_t Bios::open_event(uint32_t cls, uint32_t spec, uint32_t mode, uint32_t func) {
    for (int i = 0; i < kMaxEvents; ++i) {
        auto& e = events_[static_cast<std::size_t>(i)];
        if (e.status == 0) {
            e = {cls, spec, mode, func, kEvStWait};
            return 0xF1000000u | static_cast<uint32_t>(i);
        }
    }
    return 0xFFFFFFFFu;
}

int Bios::event_index(uint32_t h) const {
    if ((h & 0xFFFF0000u) != 0xF1000000u) return -1;
    int i = static_cast<int>(h & 0xFFFF);
    return i < kMaxEvents && events_[static_cast<std::size_t>(i)].status ? i : -1;
}

void Bios::deliver_event(uint32_t cls, uint32_t spec) {
    for (auto& e : events_) {
        if (e.status != kEvStActive || e.cls != cls || e.spec != spec) continue;
        if (e.mode == kEvMdNoIntr) {
            e.status = kEvStAlready;
        } else if (e.mode == kEvMdIntr && e.func) {
            RecompContext& c = m_.ctx();
            SavedRegs saved(c);
            c.r[31] = kGameReturnSentinel;
            dw3_call(&c, e.func);
            saved.restore(c);
        }
    }
}

void Bios::wait_until(RecompContext& c, const std::function<bool()>& done) {
    // Kernel busy-waits: advance emulated time event by event so interrupts keep flowing.
    while (!done()) {
        if (c.next_event == INT64_MAX) fatal("WaitEvent would wait forever (no pending hardware events)");
        if (c.cycles < c.next_event) c.cycles = c.next_event;
        dw3_service_events(&c);
    }
}

// ---------------------------------------------------------------------------------------------
// Interrupts

void Bios::poll_pads() {
    if (!pad_started_) return;
    uint8_t* ram = m_.bus().ram();
    for (int p = 0; p < 2; ++p) {
        if (!pad_buf_[p] || pad_size_[p] < 4) continue;
        uint8_t* b = ram + (dw3_phys(pad_buf_[p]) & 0x1FFFFF);
        const PadState& s = pads[static_cast<std::size_t>(p)];
        b[0] = s.connected ? 0x00 : 0xFF;
        b[1] = 0x41;  // digital pad, 1 halfword of data
        b[2] = static_cast<uint8_t>(s.buttons);
        b[3] = static_cast<uint8_t>(s.buttons >> 8);
    }
    if (outdated_pad_dest_) {
        uint32_t v = static_cast<uint32_t>(static_cast<uint16_t>(~pads[0].buttons)) |
                     (static_cast<uint32_t>(static_cast<uint16_t>(~pads[1].buttons)) << 16);
        dw3_write32(&m_.ctx(), outdated_pad_dest_, v);
    }
}

void Bios::run_int_chains(RecompContext& c) {
    // Each SysEnqIntRP node: +0 next, +4 func2, +8 func1. func2(v0) runs when func1 returns nonzero.
    for (uint32_t head : int_chain_) {
        for (uint32_t node = head, guard = 0; node && guard < 64; ++guard) {
            uint32_t next = dw3_read32(&c, node), f2 = dw3_read32(&c, node + 4), f1 = dw3_read32(&c, node + 8);
            if (f1) {
                c.r[31] = kGameReturnSentinel;
                dw3_call(&c, f1);
                uint32_t v0 = c.r[2];
                if (v0 && f2) {
                    c.r[4] = v0;
                    c.r[31] = kGameReturnSentinel;
                    dw3_call(&c, f2);
                }
            }
            node = next;
        }
    }
}

void Bios::interrupt(RecompContext& c) {
    if (in_exception_) return;
    in_exception_ = true;
    ++interrupts_;
    SavedRegs saved(c);
    uint32_t sr = c.cop0[12];
    c.cop0[12] = (sr & ~0x3Fu) | ((sr << 2) & 0x3Cu);  // push mode/IE stack: interrupts off
    c.cop0[13] = (c.cop0[13] & ~0x7Cu);                // ExcCode 0 = interrupt
    try {
        uint32_t stat = m_.bus().read(kIStat, 4) & m_.bus().read(kIMask, 4);
        // Kernel-owned work first: pad polling on VBLANK, root counter events.
        if (stat & 1u) {
            poll_pads();
            if (clear_pad_ && pad_started_) m_.bus().write(kIStat, ~1u, 4);
        }
        run_int_chains(c);
        static constexpr uint32_t rcnt_bits[4] = {1u << 4, 1u << 5, 1u << 6, 1u << 0};
        for (int t = 0; t < 4; ++t) {
            if (!(stat & rcnt_bits[t])) continue;
            deliver_event(kClassRCnt + static_cast<uint32_t>(t), kSpecInt);
            if (clear_rcnt_[static_cast<std::size_t>(t)]) m_.bus().write(kIStat, ~rcnt_bits[t], 4);
        }
        // Custom exit (HookEntryInt): "longjmp" into the registered buffer with v0 = 1.
        if (hook_) {
            c.r[31] = dw3_read32(&c, hook_ + 0);
            c.r[29] = dw3_read32(&c, hook_ + 4);
            c.r[30] = dw3_read32(&c, hook_ + 8);
            for (int k = 0; k < 8; ++k) c.r[16 + k] = dw3_read32(&c, hook_ + 12 + 4 * static_cast<uint32_t>(k));
            c.r[28] = dw3_read32(&c, hook_ + 44);
            c.r[2] = 1;
            dw3_call(&c, c.r[31]);
        }
    } catch (const ReturnFromException&) {
    }
    saved.restore(c);
    c.cop0[12] = sr;
    in_exception_ = false;
}

void Bios::syscall(RecompContext& c, uint32_t pc) {
    switch (c.r[4]) {
        case 0: break;
        case 1:  // EnterCriticalSection: returns 1 if interrupts were enabled
            c.r[2] = (c.cop0[12] & (kSrIEc | kSrIM2)) == (kSrIEc | kSrIM2) ? 1u : 0u;
            c.cop0[12] &= ~(kSrIEc | kSrIM2);
            break;
        case 2:  // ExitCriticalSection
            c.cop0[12] |= kSrIEc | kSrIM2;
            m_.request_service();
            break;
        default: fatal("unsupported syscall(" + std::to_string(c.r[4]) + ") at " + hexs(pc));
    }
}

void Bios::card_async(uint32_t spec_sw, uint32_t spec_hw) {
    // No memory card yet: completion is reported asynchronously, as the real card driver does.
    m_.scheduler().schedule_in(20000, [this, spec_sw, spec_hw](int64_t) {
        deliver_event(kClassSwCard, spec_sw);
        deliver_event(kClassHwCard, spec_hw);
    });
}

void Bios::unknown(uint32_t table, uint32_t fn) {
    char b[64];
    std::snprintf(b, sizeof(b), "unimplemented BIOS function %02X:%02X", table, fn);
    fatal(b);
}

// ---------------------------------------------------------------------------------------------
// A0/B0/C0 dispatch

void Bios::call(RecompContext& c, uint32_t table, uint32_t fn) {
    ++calls_;
    uint32_t a0 = c.r[4], a1 = c.r[5], a2 = c.r[6], a3 = c.r[7];
    uint32_t& v0 = c.r[2];
    if (log) {
        char b[96];
        std::snprintf(b, sizeof(b), "BIOS %02X:%02X(%08X, %08X, %08X, %08X)", table, fn, a0, a1, a2, a3);
        log(b);
    }
    if (table == 0xA0) {
        switch (fn) {
            case 0x39: return;                                  // InitHeap: malloc is never used
            case 0x44: return;                                  // FlushCache: no I-cache to flush
            case 0x49: m_.bus().write(kGp0, a0, 4); return;     // GPU_cw
            case 0x70: return;                                  // _bu_init
            case 0x72: return;                                  // CdRemove: no kernel CD handlers
            case 0xAB: v0 = 1; card_async(kSpecTimeout, kSpecTimeout); return;  // _card_info
            case 0xAC: v0 = 1; card_async(kSpecTimeout, kSpecTimeout); return;  // _card_load
        }
    } else if (table == 0xB0) {
        switch (fn) {
            case 0x07: {  // DeliverEvent(class, spec)
                SavedRegs saved(c);
                deliver_event(a0, a1);
                saved.restore(c);
                return;
            }
            case 0x08: v0 = open_event(a0, a1, a2, a3); return;  // OpenEvent
            case 0x09: {                                          // CloseEvent
                int i = event_index(a0);
                if (i >= 0) events_[static_cast<std::size_t>(i)].status = 0;
                v0 = i >= 0 ? 1u : 0u;
                return;
            }
            case 0x0A: {  // WaitEvent
                int i = event_index(a0);
                if (i < 0) {
                    v0 = 0;
                    return;
                }
                auto& e = events_[static_cast<std::size_t>(i)];
                SavedRegs saved(c);
                wait_until(c, [&] { return e.status == kEvStAlready; });
                saved.restore(c);
                e.status = kEvStActive;
                v0 = 1;
                return;
            }
            case 0x0B: {  // TestEvent
                int i = event_index(a0);
                v0 = 0;
                if (i >= 0 && events_[static_cast<std::size_t>(i)].status == kEvStAlready) {
                    events_[static_cast<std::size_t>(i)].status = kEvStActive;
                    v0 = 1;
                }
                return;
            }
            case 0x0C: {  // EnableEvent
                int i = event_index(a0);
                if (i >= 0) events_[static_cast<std::size_t>(i)].status = kEvStActive;
                v0 = i >= 0 ? 1u : 0u;
                return;
            }
            case 0x12:  // InitPAD(buf1, siz1, buf2, siz2)
                pad_buf_[0] = a0;
                pad_size_[0] = a1;
                pad_buf_[1] = a2;
                pad_size_[1] = a3;
                v0 = 2;
                return;
            case 0x13: pad_started_ = true; m_.bus().write(kIMask, m_.bus().read(kIMask, 4) | 1u, 4); return;  // StartPAD
            case 0x15:  // OutdatedPadInitAndStart(type, button_dest, ...)
                outdated_pad_dest_ = a1;
                pad_started_ = true;
                return;
            case 0x17: throw ReturnFromException{};          // ReturnFromException
            case 0x18: hook_ = 0; return;                    // ResetEntryInt
            case 0x19: hook_ = a0; return;                   // HookEntryInt
            case 0x32: v0 = 0xFFFFFFFFu; return;             // FileOpen: no files/cards yet
            case 0x33: v0 = 0xFFFFFFFFu; return;             // FileSeek
            case 0x34: v0 = 0xFFFFFFFFu; return;             // FileRead
            case 0x35:  // FileWrite: fd 1 is the debug console (printf)
                if (a0 == 1) {
                    std::string text;
                    for (uint32_t k = 0; k < a2 && k < 4096; ++k) text += static_cast<char>(dw3_read8(&c, a1 + k));
                    tty_ += text;
                    for (auto nl = tty_.find('\n'); nl != std::string::npos; nl = tty_.find('\n')) {
                        if (on_tty) on_tty(tty_.substr(0, nl));
                        tty_.erase(0, nl + 1);
                    }
                    v0 = a2;
                    return;
                }
                v0 = 0xFFFFFFFFu;
                return;
            case 0x36: v0 = a0; return;                      // FileClose
            case 0x42: v0 = 0; return;                       // firstfile: none
            case 0x43: v0 = 0; return;                       // nextfile
            case 0x4A: return;                               // InitCard
            case 0x4B: return;                               // StartCard
            case 0x4C: return;                               // StopCard
            case 0x4E: v0 = 1; card_async(kSpecTimeout, kSpecTimeout); return;  // write_card_sector
            case 0x4F: v0 = 1; card_async(kSpecTimeout, kSpecTimeout); return;  // read_card_sector
            case 0x50: return;                               // allow_new_card
            // GetC0Table / GetB0Table: libapi/libcard read kernel tables to patch the kernel's exception
            // handler in RAM. There is no kernel code here: the (zeroed) tables make the guarded patch
            // find no match, and libcard's unconditional copy lands in unused kernel RAM.
            case 0x56: v0 = 0x00000674; return;
            case 0x57: v0 = 0x00000874; return;
            case 0x5B: clear_pad_ = a0 != 0; return;         // ChangeClearPad
        }
    } else if (table == 0xC0) {
        switch (fn) {
            case 0x02: {  // SysEnqIntRP(priority, struc)
                if (a0 < 4) {
                    dw3_write32(&c, a1, int_chain_[a0]);
                    int_chain_[a0] = a1;
                }
                return;
            }
            case 0x03: {  // SysDeqIntRP(priority, struc)
                if (a0 >= 4) return;
                if (int_chain_[a0] == a1) {
                    int_chain_[a0] = dw3_read32(&c, a1);
                    return;
                }
                for (uint32_t cur = int_chain_[a0], guard = 0; cur && guard < 64; ++guard) {
                    uint32_t next = dw3_read32(&c, cur);
                    if (next == a1) {
                        dw3_write32(&c, cur, dw3_read32(&c, a1));
                        return;
                    }
                    cur = next;
                }
                return;
            }
            case 0x0A: {  // ChangeClearRCnt(t, flag) -> old flag
                if (a0 < 4) {
                    v0 = clear_rcnt_[a0] ? 1u : 0u;
                    clear_rcnt_[a0] = a1 != 0;
                }
                return;
            }
        }
    }
    unknown(table, fn);
}

}  // namespace dw3
