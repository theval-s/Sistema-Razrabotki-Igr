#include <engine/core/Uuid.hpp>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <random>

#include <stduuid/uuid.h>

namespace engine {

bool IsUuid(std::string_view text) noexcept {
    if (text.size() != 36) return false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (text[i] != '-') return false;
        } else if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f'))) return false;
    }
    return true;
}

std::optional<std::array<std::uint8_t, 16>> ParseUuidBytes(std::string_view text) noexcept {
    if (!IsUuid(text)) return std::nullopt;
    const auto parsed = uuids::uuid::from_string(text);
    if (!parsed) return std::nullopt;
    std::array<std::uint8_t, 16> bytes{};
    std::ranges::transform(parsed->as_bytes(), bytes.begin(),
                           [](std::byte byte) { return std::to_integer<std::uint8_t>(byte); });
    return bytes;
}

std::string FormatUuidBytes(const std::array<std::uint8_t, 16>& bytes) {
    return uuids::to_string(uuids::uuid(bytes.begin(), bytes.end()));
}

std::string NewUuid() {
    //mt for faster gen
    thread_local std::mt19937 engine = [] {
        std::random_device device;
        std::array<std::uint32_t, std::mt19937::state_size> seed{};
        std::ranges::generate(seed, std::ref(device));
        std::seed_seq sequence(seed.begin(), seed.end());
        return std::mt19937(sequence);
    }();
    thread_local uuids::uuid_random_generator generator{engine};
    return uuids::to_string(generator());
}

} // namespace engine
