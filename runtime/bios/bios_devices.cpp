// Kernel device table (DCB array) and the BIOS file functions that dispatch through it.
//
// Layout follows psx-spx "BIOS File Functions": the kernel keeps the DCB array address/size at 0x150/0x154
// and the FCB array address/size at 0x140/0x144. libcard's firstfile wrapper in the game scans this table
// by device name and patches the DCB firstfile pointer, so the table must exist in RAM and every handler
// must be called through the pointer read from RAM, not through a C++ shortcut.
// DCB (0x50 bytes): +00 name, +04 flags, +08 sector size, +0C description, +10 init, +14 open, +18 inout,
// +1C close, +20 ioctl, +24 read, +28 write, +2C erase, +30 undelete, +34 firstfile, +38 nextfile,
// +3C format, +40 chdir, +44 rename, +48 remove, +4C testdevice.

#include <cstring>

#include "dw3/bios.h"

namespace dw3 {

namespace {

// Pseudo BIOS table used by the native handler stubs to re-enter the Bios object.
constexpr uint32_t kNativeTable = 0xDE;

template <uint32_t Id>
void native_stub(RecompContext* ctx) {
    dw3_bios_call(ctx, kNativeTable, Id);
}


}  // namespace

void Bios::init_kernel_devices() {
    uint8_t* ram = m_.bus().ram();
    auto w32 = [&](uint32_t a, uint32_t v) { std::memcpy(ram + a, &v, 4); };
    auto put_str = [&](uint32_t a, const char* s) { std::memcpy(ram + a, s, std::strlen(s) + 1); };

    m_.register_native(native_addr(kNativeNop), native_stub<kNativeNop>);
    m_.register_native(native_addr(kNativeBuFirst), native_stub<kNativeBuFirst>);
    m_.register_native(native_addr(kNativeBuNext), native_stub<kNativeBuNext>);

    struct Dev {
        const char *name, *desc;
        uint32_t flags, sector;
    };
    static constexpr Dev devs[kDcbCount] = {{"tty", "CONSOLE", 3, 1}, {"cdrom", "CD-ROM", 1, 0x800},
                                            {"bu", "MEMORY CARD", 1, 0x80}};
    uint32_t str = kDevNames;
    for (uint32_t i = 0; i < kDcbCount; ++i) {
        uint32_t d = kDcbBase + i * kDcbSize, name = str, desc = str + 8;
        put_str(name, devs[i].name);
        put_str(desc, devs[i].desc);
        str += 0x18;
        w32(d + 0x00, name);
        w32(d + 0x04, devs[i].flags);
        w32(d + 0x08, devs[i].sector);
        w32(d + 0x0C, desc);
        for (uint32_t off = 0x10; off < kDcbSize; off += 4) w32(d + off, native_addr(kNativeNop));
        if (i == 2) {
            w32(d + 0x34, native_addr(kNativeBuFirst));
            w32(d + 0x38, native_addr(kNativeBuNext));
        }
    }
    w32(0x150, kDcbBase);
    w32(0x154, kDcbCount * kDcbSize);
    w32(0x140, kFcbBase);
    w32(0x144, kFcbCount * kFcbSize);
    std::memset(ram + kFcbBase, 0, kFcbCount * kFcbSize);
}

uint32_t Bios::find_dcb(const std::string& device) {
    uint32_t base = dw3_read32(&m_.ctx(), 0x150), size = dw3_read32(&m_.ctx(), 0x154);
    for (uint32_t off = 0; off + kDcbSize <= size; off += kDcbSize) {
        uint32_t name = dw3_read32(&m_.ctx(), base + off);
        std::string s;
        for (uint32_t i = 0; i < 16; ++i) {
            char ch = static_cast<char>(dw3_read8(&m_.ctx(), name + i));
            if (!ch) break;
            s += ch;
        }
        if (s == device) return base + off;
    }
    return 0;
}

// Calls a DCB function pointer read from RAM with (a0, a1, a2) and returns v0; caller-saved state is kept.
uint32_t Bios::call_dcb_fn(RecompContext& c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2) {
    if (!fn) return 0;
    uint32_t saved_r[32];
    std::memcpy(saved_r, c.r, sizeof(saved_r));
    c.r[4] = a0;
    c.r[5] = a1;
    c.r[6] = a2;
    c.r[31] = 0;
    dw3_call(&c, fn);
    uint32_t v0 = c.r[2];
    std::memcpy(c.r, saved_r, sizeof(saved_r));
    return v0;
}

// B0:42 firstfile(path, dirent): resolve "bu00:NAME" -> device "bu", port 0x00, name "NAME", then
// DCB.firstfile(fcb, name, dirent). Returns dirent or 0.
uint32_t Bios::file_first_via_dcb(RecompContext& c, uint32_t path) {
    uint32_t entry = c.r[5];
    std::string p = read_cstr(c, path);
    auto colon = p.find(':');
    std::size_t dev_end = p.find_first_of("0123456789:");
    if (colon == std::string::npos || dev_end == std::string::npos) return errno_ = 0x13, 0;
    uint32_t dcb = find_dcb(p.substr(0, dev_end));
    if (!dcb) return errno_ = 0x13, 0;
    uint32_t port = dev_end + 1 < colon ? static_cast<uint32_t>((p[dev_end] - '0') * 16) : 0;
    uint32_t fcb = kFcbBase;
    dw3_write32(&c, fcb + 4, port);  // diskid
    search_.device = p.substr(0, dev_end);
    return call_dcb_fn(c, dw3_read32(&c, dcb + 0x34), fcb, path + static_cast<uint32_t>(colon) + 1, entry);
}

// B0:43 nextfile(dirent): continues the search started by the last firstfile.
uint32_t Bios::file_next_via_dcb(RecompContext& c, uint32_t entry) {
    uint32_t dcb = search_.device.empty() ? 0 : find_dcb(search_.device);
    if (!dcb) return errno_ = 0x13, 0;
    return call_dcb_fn(c, dw3_read32(&c, dcb + 0x38), kFcbBase, entry, 0);
}

void Bios::native_device_call(RecompContext& c, uint32_t id) {
    uint32_t a0 = c.r[4], a1 = c.r[5], a2 = c.r[6];
    switch (id) {
        case kNativeBuFirst: c.r[2] = bu_first(c, a0, a1, a2); break;
        case kNativeBuNext: c.r[2] = bu_next(c, a0, a1); break;
        default: c.r[2] = 0; break;
    }
}

}  // namespace dw3
