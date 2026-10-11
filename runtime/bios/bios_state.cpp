// Native BIOS save state. Host wiring (log/tty/card-write callbacks, pad input) and the memory card images are
// owned by the host and are not part of it.

#include "dw3/bios.h"

namespace dw3 {

void Bios::io(StateIo& s) {
    if (!s.loading() && (host_depth_ != 0 || in_exception_))
        throw StateError("BIOS state cannot be saved while a kernel frame is active");
    StateIo::Section sec(s, "BIOS");
    for (auto& e : events_) {
        s.io(e.cls);
        s.io(e.spec);
        s.io(e.mode);
        s.io(e.func);
        s.io(e.status);
    }
    s.io(int_chain_);
    s.io(hook_);
    for (auto& v : pad_buf_) s.io(v);
    for (auto& v : pad_size_) s.io(v);
    s.io(outdated_pad_dest_);
    s.io(pad_started_);
    s.io(clear_pad_);
    s.io(clear_rcnt_);
    for (auto& f : files_) {
        s.io(f.used);
        s.io(f.slot);
        s.io(f.name);
        s.io(f.pos);
        s.io(f.mode);
    }
    s.io(search_.active);
    s.io(search_.slot);
    s.io(search_.last_block);
    s.io(search_.pattern);
    s.io(search_.device);
    s.io(errno_);
    s.io(tty_);
    s.io(interrupts_);
    s.io(calls_);
    s.list(pending_cards_, [&](PendingCard& p) {
        s.io(p.spec);
        s.io(p.software);
        m_.scheduler().io_event(s, p.ev, [this, &p] { return card_completion(&p); });
    });
    if (s.loading()) {
        host_depth_ = 0;
        in_exception_ = false;
    }
}

}  // namespace dw3
