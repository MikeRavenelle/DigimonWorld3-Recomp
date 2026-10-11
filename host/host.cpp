#include "dw3/host.h"

#include <map>
#include <span>
#include <utility>

#include "dw3/memcard.h"
#include "dw3/recomp.h"
#include "dw3/platform.h"
#include "psxdisc/iso9660.h"

namespace dw3::host {

namespace {

struct HostFatal {
    std::string message;
};

// Feeds raw disc sectors to the emulated CD-ROM drive.
class DiscSource final : public SectorSource {
public:
    explicit DiscSource(psxdisc::Disc& d) : disc_(d) {}
    uint32_t sector_count() const override { return disc_.sector_count(); }
    bool read_raw(uint32_t lba, uint8_t out[2352]) override {
        return disc_.read_raw(lba, std::span<uint8_t, psxdisc::kRawSectorSize>(out, psxdisc::kRawSectorSize));
    }

private:
    psxdisc::Disc& disc_;
};

std::vector<uint8_t> read_boot_exe(psxdisc::Disc& disc) {
    auto root = psxdisc::read_root(disc);
    if (!root) return {};
    for (const auto& f : psxdisc::walk(disc, root->lba, root->size))
        if (f.path == "SLUS_014.36") return disc.read_file(f.lba, f.size);
    return {};
}

}  // namespace

struct Host::Impl {
    Config cfg;
    std::unique_ptr<DiscSource> disc_source;
    std::array<std::vector<uint8_t>, 2> image;
    std::array<std::filesystem::path, 2> path;  // empty = memory only
    std::array<bool, 2> dirty{};
    std::array<int, 2> countdown{};
    std::map<uint32_t, uint64_t> unmapped;  // physical address -> access count

    void log(const std::string& s) const {
        if (cfg.log) cfg.log(s);
    }

    // Loads (or, when `create`, formats and creates) the card file; returns false when the slot stays empty.
    bool open_file(int slot, const std::filesystem::path& p, bool create) {
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
        auto s = static_cast<std::size_t>(slot);
        path[s] = p;
        if (auto loaded = memcard::load_card_file(p)) {
            image[s] = std::move(*loaded);
            return true;
        }
        if (std::filesystem::exists(p, ec)) {
            log("memory card " + p.string() + " is not a valid 128 KiB image; slot " + std::to_string(slot + 1) +
                " left empty");
            path[s].clear();
            return false;
        }
        if (!create) {
            path[s].clear();
            return false;
        }
        image[s].assign(memcard::kCardSize, 0);
        memcard::format_memory_card(std::span<uint8_t, memcard::kCardSize>(image[s].data(), image[s].size()));
        flush(slot);
        return true;
    }
    void flush(int slot) {
        auto s = static_cast<std::size_t>(slot);
        if (image[s].empty() || path[s].empty()) return;
        if (!memcard::save_card_file(path[s], image[s])) log("cannot write memory card " + path[s].string());
        dirty[s] = false;
    }
    void touch(int slot) {
        auto s = static_cast<std::size_t>(slot & 1);
        dirty[s] = true;
        countdown[s] = 30;
    }
    void tick() {
        for (std::size_t s = 0; s < 2; ++s)
            if (dirty[s] && --countdown[s] <= 0) flush(static_cast<int>(s));
    }
    void flush_all() {
        for (std::size_t s = 0; s < 2; ++s)
            if (dirty[s]) flush(static_cast<int>(s));
    }
};

Host::Host() : impl_(std::make_unique<Impl>()) {}
Host::~Host() = default;

const std::vector<uint8_t>& Host::card_image(int slot) const { return impl_->image[static_cast<std::size_t>(slot)]; }

int Host::card_count() const { return (impl_->image[0].empty() ? 0 : 1) + (impl_->image[1].empty() ? 0 : 1); }

std::unique_ptr<Host> Host::create(const Config& cfg, std::string& error) {
    std::unique_ptr<Host> h(new Host());
    Impl& im = *h->impl_;
    im.cfg = cfg;
    h->disc_ = psxdisc::Disc::open(cfg.disc_path);
    if (!h->disc_) {
        error = "cannot open disc image " + cfg.disc_path;
        return nullptr;
    }
    h->exe_ = read_boot_exe(*h->disc_);
    if (h->exe_.empty()) {
        error = "SLUS_014.36 not found on disc";
        return nullptr;
    }
    h->machine_ = std::make_unique<Machine>(std::span<const RecompSegment>(dw3_segments, dw3_segment_count));
    h->bios_ = std::make_unique<Bios>(*h->machine_);
    Machine& m = *h->machine_;
    Bios& bios = *h->bios_;
    Host* self = h.get();
    im.disc_source = std::make_unique<DiscSource>(*h->disc_);
    m.cdrom().set_disc(im.disc_source.get());
    m.spu().start(m.scheduler());  // timed sample generation (a periodic event; plain Machine tests stay quiet)

    if (cfg.card_mode == CardMode::Blank) {
        if (cfg.blank_card.size() == memcard::kCardSize) {
            im.image[0] = cfg.blank_card;
        } else {
            im.image[0].assign(memcard::kCardSize, 0);
            memcard::format_memory_card(std::span<uint8_t, memcard::kCardSize>(im.image[0].data(), memcard::kCardSize));
        }
        m.sio().attach_card(0, im.image[0]);
    } else if (cfg.card_mode == CardMode::Files) {
        auto dir = platform::user_data_dir() / "memcards";
        std::filesystem::path p1 = cfg.card_path[0].empty() ? dir / "slot1.mcd" : std::filesystem::path(cfg.card_path[0]);
        if (im.open_file(0, p1, true)) {
            m.sio().attach_card(0, im.image[0]);
            im.log("memory card 1: " + p1.string());
        }
        // Slot 2 is inserted only if its file exists, so "no card" stays available to the game.
        std::filesystem::path p2 = cfg.card_path[1].empty() ? dir / "slot2.mcd" : std::filesystem::path(cfg.card_path[1]);
        std::error_code ec;
        if (std::filesystem::exists(p2, ec) && im.open_file(1, p2, false)) {
            m.sio().attach_card(1, im.image[1]);
            im.log("memory card 2: " + p2.string());
        }
    }
    if (cfg.card_mode == CardMode::Files) {
        m.sio().on_card_write = [self](int slot, uint32_t) { self->impl_->touch(slot); };
        bios.on_card_write = [self](int slot) { self->impl_->touch(slot); };
    }

    if (cfg.trace_bios) bios.log = [self](const std::string& s) { self->impl_->log(s); };
    bios.on_tty = [self](const std::string& s) {
        self->tty_ += s;
        self->tty_ += '\n';
        if (self->on_tty) self->on_tty(s);
    };
    if (cfg.trace_hi) {
        m.bus().trace_lo = cfg.trace_lo;
        m.bus().trace_hi = cfg.trace_hi;
        m.bus().on_trace = [self](uint32_t a, int size, bool write, uint32_t v) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "io %s%d %08X %s %08X @%lld", write ? "W" : "R", size * 8, a,
                          write ? "<-" : "->", v, static_cast<long long>(self->machine_->ctx().cycles));
            self->impl_->log(buf);
        };
    }
    m.bus().on_unmapped = [self](uint32_t addr, int size, bool write, uint32_t value) {
        if (self->impl_->unmapped[addr]++ == 0) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "unmapped %s%d %08X", write ? "W" : "R", size * 8, addr);
            std::string s = buf;
            if (write) s += " = " + std::to_string(value);
            self->impl_->log(s);
        }
    };
    return h;
}

RunResult Host::run(std::function<void(uint64_t frame)> frame_hook) {
    Impl& im = *impl_;
    machine_->video().on_frame = [this, hook = std::move(frame_hook)](uint64_t frame) {
        impl_->tick();
        if (hook) hook(frame);
    };
    dw3::set_fatal_handler([](const std::string& msg) { throw HostFatal{msg}; });
    RunResult r;
    try {
        uint32_t entry = bios_->load_exe(exe_);
        bios_->run(entry);
        r.end = RunResult::End::Returned;
    } catch (const StopRun&) {
        r.end = RunResult::End::Stopped;
    } catch (const HostFatal& f) {
        r.end = RunResult::End::Fatal;
        r.fatal_message = f.message;
    }
    machine_->video().on_frame = nullptr;
    dw3::set_fatal_handler(nullptr);
    im.flush_all();
    return r;
}

}  // namespace dw3::host
