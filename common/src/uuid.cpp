// common/src/uuid.cpp
#include <common/uuid.hpp>

#include <array>
#include <iomanip>
#include <random>
#include <sstream>

namespace uuid {

// Each thread gets its own Mersenne Twister seeded from a non-deterministic
// source. This avoids both mutex contention and the overhead of seeding on
// every call.
static thread_local std::mt19937 rng{std::random_device{}()};

std::string generate() {
    // Generate 16 random bytes.
    std::uniform_int_distribution<unsigned int> byte_dist(0, 255);

    std::array<uint8_t, 16> bytes{};
    for (auto& b : bytes) {
        b = static_cast<uint8_t>(byte_dist(rng));
    }

    // Set version bits: version 4 → top nibble of byte[6] = 0100
    bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0Fu) | 0x40u);
    // Set variant bits: RFC 4122 variant → top two bits of byte[8] = 10xx
    bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3Fu) | 0x80u);

    // Format as "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            oss << '-';
        }
        oss << std::setw(2) << static_cast<unsigned int>(bytes[i]);
    }
    return oss.str();
}

} // namespace uuid
