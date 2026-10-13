// SHA-256 (FIPS 180-4) for verifying the disc executable and downloaded files.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace dw3setup {

class Sha256 {
public:
    Sha256();
    void update(std::span<const uint8_t> data);
    // Lower-case hex of the digest; the object must not be used afterwards.
    std::string finish_hex();

private:
    void compress(const uint8_t* block);
    std::array<uint32_t, 8> state_;
    std::array<uint8_t, 64> buffer_{};
    std::size_t buffered_ = 0;
    uint64_t total_bytes_ = 0;
};

std::string sha256_hex(std::span<const uint8_t> data);
// Hashes a file; false if it cannot be read.
bool sha256_file(const std::filesystem::path& path, std::string& hex);

}  // namespace dw3setup
