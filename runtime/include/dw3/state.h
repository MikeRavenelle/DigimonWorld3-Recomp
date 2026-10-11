#pragma once

// Save-state serialization. Each hardware block describes its state once, in a single `io(StateIo&)` method
// that is used both for saving and for loading, so the two directions cannot drift apart:
//
//     void Foo::io(StateIo& s) {
//         StateIo::Section sec(s, "FOO0", 2);          // current layout version 2
//         s.io(reg_);                    // integers, bool, float and enums
//         s.io(buffer_);                 // std::array / std::vector of those (vectors are length-prefixed)
//         s.list(queue_, [&](Item& i) { s.io(i.a); s.io(i.b); });   // containers of structs
//         s.io_since(2, new_reg_, uint32_t{7});   // field added in version 2; older states get 7
//         if (s.loading()) rebuild_derived_state();
//     }
//
// Layout rules, so that states stay loadable as the code evolves:
//  - Values are little-endian with fixed widths. Only fixed-width types are stored: no struct dumps, no padding,
//    no size_t, no pointers or function pointers. Enums are stored as 32-bit integers.
//  - Everything lives in sections (tag, layout version, length). A reader skips sections it does not know, so
//    new sections can be added anywhere; a section is found by scanning forward from the current position.
//  - A block that gains a field bumps its section version and reads the field with io_since(); states written by
//    an older build load with the default. A section older than the oldest layout still supported (`min_version`)
//    or newer than this build understands is an error that names the section.
//
// Loading failures throw StateError.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace dw3 {

class StateError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class StateIo {
public:
    static constexpr std::size_t kMaxElements = std::size_t{1} << 28;  // sanity bound for length prefixes

    StateIo() = default;                              // writer
    explicit StateIo(std::span<const uint8_t> data);  // reader over `data` (not copied; must outlive the reader)

    bool loading() const { return loading_; }
    const std::vector<uint8_t>& written() const { return out_; }
    std::vector<uint8_t> take() { return std::move(out_); }
    bool at_end() const { return !loading_ || pos_ == limit(); }

    // Raw bytes (not length-prefixed).
    void bytes(void* p, std::size_t n);

    template <class T>
    std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>> io(T& v) {
        static_assert(sizeof(T) <= 8);
        using U = std::make_unsigned_t<T>;
        uint8_t b[sizeof(T)];
        if (loading_) {
            bytes(b, sizeof(T));
            U u = 0;
            for (std::size_t i = 0; i < sizeof(T); ++i) u = static_cast<U>(u | (static_cast<U>(b[i]) << (8 * i)));
            v = static_cast<T>(u);
        } else {
            auto u = static_cast<U>(v);
            for (std::size_t i = 0; i < sizeof(T); ++i) b[i] = static_cast<uint8_t>(u >> (8 * i));
            bytes(b, sizeof(T));
        }
    }
    void io(bool& v) {
        uint8_t b = v ? 1 : 0;
        io(b);
        if (loading_) v = b != 0;
    }
    void io(float& v) {
        uint32_t u = 0;
        if (!loading_) std::memcpy(&u, &v, 4);
        io(u);
        if (loading_) std::memcpy(&v, &u, 4);
    }
    template <class E>
    std::enable_if_t<std::is_enum_v<E>> io(E& v) {
        static_assert(sizeof(E) <= 4);
        int32_t i = static_cast<int32_t>(v);
        io(i);
        if (loading_) v = static_cast<E>(i);
    }
    template <class T, std::size_t N>
    void io(std::array<T, N>& a) {
        if constexpr (sizeof(T) == 1 && std::is_integral_v<T>) {
            bytes(a.data(), N);
        } else {
            for (auto& e : a) io(e);
        }
    }
    void io(std::string& s);
    template <class T>
    void io(std::vector<T>& v) {
        std::size_t n = length(v.size());
        if (loading_) v.resize(n);
        if constexpr (sizeof(T) == 1 && std::is_integral_v<T>) {
            bytes(v.data(), n);
        } else {
            for (auto& e : v) io(e);
        }
    }
    // A container (vector, deque, ...) whose elements need more than one field: `each` is called per element.
    template <class C, class F>
    void list(C& c, F&& each) {
        std::size_t n = length(c.size());
        if (loading_) {
            c.clear();
            c.resize(n);
        }
        for (auto& e : c) each(e);
    }

    // True when the field belongs to the layout that was written: always when saving, and when loading if the
    // section's version is at least `since`.
    bool has(uint32_t since) const { return !loading_ || version() >= since; }
    // A field added in section version `since`; older states leave `def` in it.
    template <class T>
    void io_since(uint32_t since, T& v, const T& def = T{}) {
        if (has(since)) io(v);
        else v = def;
    }

    // Tagged region with a layout version and a length. While loading, unknown sections in front of it are
    // skipped. A stored version above `version` or below `min_version` is an error.
    void begin(const char (&tag)[5], uint32_t version = 1, uint32_t min_version = 1);
    void end();
    // Version of the innermost open section as it was written (the current one when saving).
    uint32_t version() const { return frames_.empty() ? 0 : frames_.back().version; }
    class Section {
    public:
        Section(StateIo& s, const char (&tag)[5], uint32_t version = 1, uint32_t min_version = 1) : s_(s) {
            s_.begin(tag, version, min_version);
        }
        ~Section() noexcept(false) {
            if (!std::uncaught_exceptions()) s_.end();
        }
        Section(const Section&) = delete;
        Section& operator=(const Section&) = delete;

    private:
        StateIo& s_;
    };

    // Number of scheduler events written or read so far (checked against the scheduler's queue by the machine).
    int events = 0;

private:
    std::size_t length(std::size_t n);
    std::size_t limit() const { return frames_.empty() ? limit_ : frames_.back().end; }

    bool loading_ = false;
    std::vector<uint8_t> out_;
    std::span<const uint8_t> in_;
    std::size_t pos_ = 0, limit_ = 0;
    struct Frame {
        std::size_t start;  // writer: offset of the length word
        std::size_t end;    // reader: end of the section
        uint32_t version;
        char tag[5];
    };
    std::vector<Frame> frames_;
};

// Save-state files: a small header (magic, container version, payload encoding, identity of the game, sizes and
// CRC) followed by the zstd-compressed payload that StateIo produced. The payload may first have been run through an
// encoding that is not part of the format of the blocks (for example references into the disc, see disc_ref.h).
namespace savestate {

constexpr uint32_t kContainerVersion = 1;
enum Encoding : uint32_t { kPlain = 0, kDiscRefs = 1 };

struct Info {
    uint32_t encoding = kPlain;
    uint64_t decoded_size = 0;  // size of the payload once the encoding is undone
};

// `stored` is the payload after the encoding was applied; `info.decoded_size` is its size before.
std::vector<uint8_t> wrap(std::span<const uint8_t> stored, uint64_t identity, const Info& info = {});
// Checks the header against `identity` and returns the decompressed stored bytes, still in `info.encoding`. Throws
// StateError with a message that says what is wrong (not a save state, newer container format, other game, damaged
// file, unknown encoding).
std::vector<uint8_t> unwrap(std::span<const uint8_t> file, uint64_t identity, Info& info);

bool write_file(const std::string& path, std::span<const uint8_t> file, std::string& error);
bool read_file(const std::string& path, std::vector<uint8_t>& file, std::string& error);

}  // namespace savestate

}  // namespace dw3
