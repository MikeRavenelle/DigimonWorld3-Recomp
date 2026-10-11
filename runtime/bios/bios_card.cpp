// Native BIOS memory card and file functions on top of the card images attached to Sio.
//
// References: psx-spx "BIOS Memory Card Functions" / "BIOS File Functions" / "Memory Card Data Format";
// behaviour cross-checked against PCSX-Redux's HLE BIOS (GPL-2+) and OpenBIOS (MIT). No code was copied.
//
// Every operation is applied to the image immediately. Completion is reported a short time later with
// EvSpIOE (0x0004) success, EvSpTIMOUT (0x0100) no card, EvSpNEW (0x2000) new card (_card_info only),
// EvSpERROR (0x8000) bad sector / unformatted card. Event classes: the card driver's interrupt handler
// raises HwCARD (0xF0000011) for every low-level transfer (sector read/write, file I/O, directory reads);
// the high-level _card_info/_card_load additionally report SwCARD (0xF4000001) once their result is
// interpreted. This matches how the game waits (SwCARD for info/load, HwCARD for everything else). Source:
// psx-spx "BIOS Memory Card Functions" event list and the game's own wait code; not verified against
// OpenBIOS source text offline.

#include <algorithm>
#include <cstring>

#include "dw3/bios.h"
#include "dw3/memcard.h"

namespace dw3 {

namespace {

constexpr uint32_t kClassHwCard = 0xF0000011;
constexpr uint32_t kClassSwCard = 0xF4000001;
constexpr uint32_t kSpecIoe = 0x0004, kSpecNew = 0x2000, kSpecError = 0x8000, kSpecTimeout = 0x0100;
constexpr int64_t kCardLatency = 20000;  // cycles until the completion events fire
constexpr uint32_t kFread = 0x0001, kFwrite = 0x0002, kFcreat = 0x0200, kFasync = 0x8000;
constexpr uint32_t kFail = 0xFFFFFFFFu;
constexpr uint32_t kDirEntrySize = 0x28;

// Port number (0x00 / 0x10) to slot; -1 for anything else (multitap ports are not supported).
int slot_of_port(uint32_t port) { return port == 0x00 ? 0 : port == 0x10 ? 1 : -1; }

struct Path {
    int slot = -1;
    std::string name;
    bool ok = false;
};

Path parse_path(const std::string& s) {
    Path p;
    if (s.size() >= 5 && s.compare(0, 2, "bu") == 0 && s[4] == ':' && (s[2] == '0' || s[2] == '1') && s[3] == '0') {
        p.slot = s[2] - '0';
        p.name = s.substr(5);
        p.ok = true;
    }
    return p;
}

// '?' matches one character, '*' any run (including none).
bool wildcard(std::string_view pat, std::string_view str) {
    if (pat.empty()) return str.empty();
    if (pat[0] == '*') {
        for (std::size_t i = 0; i <= str.size(); ++i)
            if (wildcard(pat.substr(1), str.substr(i))) return true;
        return false;
    }
    if (str.empty()) return false;
    return (pat[0] == '?' || pat[0] == str[0]) && wildcard(pat.substr(1), str.substr(1));
}

}  // namespace

void Bios::card_async(uint32_t spec, bool software) {
    m_.scheduler().schedule_in(kCardLatency, [this, spec, software](int64_t) {
        if (software) deliver_event(kClassSwCard, spec);
        deliver_event(kClassHwCard, spec);
    });
}

void Bios::card_modified(int slot) {
    m_.sio().clear_card_new(slot);
    if (on_card_write) on_card_write(slot);
}

std::string Bios::read_cstr(RecompContext& c, uint32_t addr) const {
    std::string s;
    for (uint32_t i = 0; i < 128; ++i) {
        char ch = static_cast<char>(dw3_read8(&c, addr + i));
        if (!ch) break;
        s += ch;
    }
    return s;
}

bool Bios::card_dispatch(RecompContext& c, uint32_t table, uint32_t fn) {
    uint32_t a0 = c.r[4], a1 = c.r[5], a2 = c.r[6];
    uint32_t& v0 = c.r[2];
    auto slot_card = [&](int slot) { return slot >= 0 && m_.sio().has_card(slot); };

    if (table == 0xA0 && (fn == 0xAB || fn == 0xAC)) {  // _card_info(port) / _card_load(port)
        int slot = slot_of_port(a0);
        v0 = 1;
        if (!slot_card(slot)) return card_async(kSpecTimeout, true), true;
        if (fn == 0xAB) return card_async((m_.sio().card_flag(slot) & 0x08) ? kSpecNew : kSpecIoe, true), true;
        memcard::CardFs fs(m_.sio().card_data(slot));
        return card_async(fs.valid() ? kSpecIoe : kSpecError, true), true;
    }
    if (table != 0xB0) return false;

    switch (fn) {
        case 0x4E:    // write_card_sector(port, sector, src)
        case 0x4F: {  // read_card_sector(port, sector, dst)
            int slot = slot_of_port(a0);
            v0 = 1;
            if (!slot_card(slot)) return card_async(kSpecTimeout), true;
            if (a1 >= memcard::kFrameCount) return card_async(kSpecError), true;
            uint8_t* frame = m_.sio().card_data(slot).data() + a1 * memcard::kFrameSize;
            if (fn == 0x4F) {
                for (uint32_t i = 0; i < memcard::kFrameSize; ++i) dw3_write8(&c, a2 + i, frame[i]);
            } else {
                for (uint32_t i = 0; i < memcard::kFrameSize; ++i) frame[i] = dw3_read8(&c, a2 + i);
                card_modified(slot);
            }
            return card_async(kSpecIoe), true;
        }
        case 0x50: return true;  // allow_new_card: the NEW flag is cleared by the first write, as on hardware
        case 0x54: v0 = errno_; return true;  // _get_errno
        case 0x32: v0 = file_open(c, a0, a1); return true;
        case 0x33: v0 = file_seek(a0, static_cast<int32_t>(a1), a2); return true;
        case 0x34: v0 = file_transfer(c, a0, a1, a2, false); return true;
        case 0x35:
            if (a0 == 1) return false;  // console (printf)
            v0 = file_transfer(c, a0, a1, a2, true);
            return true;
        case 0x36:  // FileClose
            if (a0 >= files_.size() || !files_[a0].used) return errno_ = 9, v0 = kFail, true;
            files_[a0] = {};
            v0 = a0;
            return true;
        case 0x42: v0 = file_first_via_dcb(c, a0); return true;
        case 0x43: v0 = file_next_via_dcb(c, a0); return true;
        case 0x41: case 0x44: case 0x45: v0 = file_misc(c, fn, a0, a1); return true;
        default: return false;
    }
}

uint32_t Bios::file_open(RecompContext& c, uint32_t path_addr, uint32_t mode) {
    Path p = parse_path(read_cstr(c, path_addr));
    if (!p.ok || !m_.sio().has_card(p.slot)) return errno_ = 0x13, kFail;
    memcard::CardFs fs(m_.sio().card_data(p.slot));
    if (!fs.valid()) return errno_ = 0x13, kFail;
    std::size_t fd = 2;
    while (fd < files_.size() && files_[fd].used) ++fd;
    if (fd == files_.size()) return errno_ = 0x18, kFail;
    if (mode & kFcreat) {
        auto st = fs.create(p.name, static_cast<int>(mode >> 16));
        if (st != memcard::Status::Ok) return errno_ = static_cast<uint32_t>(st), kFail;
        card_modified(p.slot);
    } else if (!fs.find(p.name)) {
        return errno_ = 0x02, kFail;
    }
    files_[fd] = {true, p.slot, p.name, 0, mode};
    errno_ = 0;
    if (mode & kFasync) card_async(kSpecIoe);
    return static_cast<uint32_t>(fd);
}

uint32_t Bios::file_seek(uint32_t fd, int32_t off, uint32_t whence) {
    if (fd >= files_.size() || !files_[fd].used) return errno_ = 9, kFail;
    auto& f = files_[fd];
    memcard::CardFs fs(m_.sio().card_data(f.slot));
    auto info = fs.find(f.name);
    if (!info) return errno_ = 0x02, kFail;
    int64_t base = whence == 0 ? 0 : whence == 1 ? f.pos : whence == 2 ? info->size : -1;
    int64_t pos = base + off;
    if (base < 0 || pos < 0 || pos > info->size) return errno_ = 0x16, kFail;
    f.pos = static_cast<uint32_t>(pos);
    return f.pos;
}

uint32_t Bios::file_transfer(RecompContext& c, uint32_t fd, uint32_t buf, uint32_t len, bool write) {
    if (fd >= files_.size() || !files_[fd].used) return errno_ = 9, kFail;
    auto& f = files_[fd];
    if (!(f.mode & (write ? kFwrite : kFread)) || len % memcard::kFrameSize != 0) return errno_ = 0x16, kFail;
    memcard::CardFs fs(m_.sio().card_data(f.slot));
    auto info = fs.find(f.name);
    if (!info) return errno_ = 0x02, kFail;
    std::vector<uint8_t> tmp(len);
    std::size_t n;
    if (write) {
        for (uint32_t i = 0; i < len; ++i) tmp[i] = dw3_read8(&c, buf + i);
        n = fs.write(*info, f.pos, tmp);
        if (n) card_modified(f.slot);
    } else {
        n = fs.read(*info, f.pos, tmp);
        for (std::size_t i = 0; i < n; ++i) dw3_write8(&c, buf + static_cast<uint32_t>(i), tmp[i]);
    }
    f.pos += static_cast<uint32_t>(n);
    errno_ = 0;
    if (f.mode & kFasync) card_async(kSpecIoe);
    return static_cast<uint32_t>(n);
}

namespace {

void fill_direntry(RecompContext& c, uint32_t addr, const memcard::FileInfo& f) {
    for (uint32_t i = 0; i < kDirEntrySize; i += 4) dw3_write32(&c, addr + i, 0);
    for (std::size_t i = 0; i < f.name.size() && i < memcard::kMaxNameLen; ++i)
        dw3_write8(&c, addr + static_cast<uint32_t>(i), static_cast<uint8_t>(f.name[i]));
    dw3_write32(&c, addr + 0x14, 0x50);  // attribute: normal file
    dw3_write32(&c, addr + 0x18, f.size);
    dw3_write32(&c, addr + 0x1C, 0);  // next: unused (the caller iterates with nextfile)
    dw3_write32(&c, addr + 0x20, f.blocks.front() * memcard::kFramesPerBlock);  // head: first sector
}

}  // namespace

// Reports the first matching file whose first block is after search_.last_block.
namespace {
std::optional<memcard::FileInfo> next_match(const memcard::CardFs& fs, const std::string& pat, int after) {
    for (auto& f : fs.list())
        if (f.blocks.front() > after && wildcard(pat, f.name)) return f;
    return std::nullopt;
}
}  // namespace

uint32_t Bios::file_next(RecompContext& c, uint32_t entry) {
    if (!search_.active || !m_.sio().has_card(search_.slot)) return 0;
    memcard::CardFs fs(m_.sio().card_data(search_.slot));
    auto f = next_match(fs, search_.pattern, search_.last_block);
    if (!f) {
        search_.active = false;
        errno_ = 0x02;
        return 0;
    }
    search_.last_block = f->blocks.front();
    fill_direntry(c, entry, *f);
    errno_ = 0;
    return entry;
}

// Native "bu" DCB handlers. fcb+4 holds the port (0x00/0x10) stored by file_first_via_dcb. The directory
// read completes later with a HwCARD event, like the real driver (TIMOUT: no card, ERROR: unformatted).
uint32_t Bios::bu_first(RecompContext& c, uint32_t fcb, uint32_t name, uint32_t entry) {
    int slot = slot_of_port(dw3_read32(&c, fcb + 4));
    std::string pattern = read_cstr(c, name);
    search_.active = false;
    if (slot < 0 || !m_.sio().has_card(slot)) return card_async(kSpecTimeout), errno_ = 0x13, 0;
    memcard::CardFs fs(m_.sio().card_data(slot));
    if (!fs.valid()) return card_async(kSpecError), errno_ = 0x13, 0;
    search_ = {true, slot, 0, pattern.empty() ? "*" : pattern, "bu"};
    uint32_t r = file_next(c, entry);
    card_async(kSpecIoe);
    return r;
}

uint32_t Bios::bu_next(RecompContext& c, uint32_t, uint32_t entry) {
    if (!search_.active || !m_.sio().has_card(search_.slot)) return card_async(kSpecTimeout), 0;
    uint32_t r = file_next(c, entry);
    card_async(kSpecIoe);
    return r;
}

uint32_t Bios::file_misc(RecompContext& c, uint32_t fn, uint32_t a0, uint32_t a1) {
    Path p = parse_path(read_cstr(c, a0));
    if (!p.ok || !m_.sio().has_card(p.slot)) return errno_ = 0x13, 0;
    auto data = m_.sio().card_data(p.slot);
    memcard::CardFs fs(data);
    memcard::Status st = memcard::Status::Ok;
    if (fn == 0x41) {  // format("bu00:")
        memcard::format_memory_card(std::span<uint8_t, memcard::kCardSize>(data.data(), memcard::kCardSize));
        for (auto& f : files_)
            if (f.used && f.slot == p.slot) f = {};
        search_.active = false;
    } else if (fn == 0x45) {  // erase(path)
        st = fs.remove(p.name);
    } else {  // rename(old, new)
        Path q = parse_path(read_cstr(c, a1));
        st = !q.ok || q.slot != p.slot ? memcard::Status::BadDevice : fs.rename(p.name, q.name);
        if (st == memcard::Status::Ok)
            for (auto& f : files_)
                if (f.used && f.slot == p.slot && f.name == p.name) f.name = q.name;
    }
    if (st != memcard::Status::Ok) return errno_ = static_cast<uint32_t>(st), 0;
    errno_ = 0;
    card_modified(p.slot);
    return 1;
}

}  // namespace dw3
