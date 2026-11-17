#pragma once
#include <engine/assets/RigAuthoring.hpp>
#include <vector>
namespace engine::detail {
struct RigProbe {
    RigInspection inspection;
    std::vector<std::uint8_t> baseColor;
    std::string textureExtension;
};
// Immutable in-memory snapshot; shares the runtime converter implementation.
bool ProbeRig(const std::vector<std::uint8_t>& bytes, RigProbe& result, std::string& error);
std::string RigDigest(std::string_view bytes);
}
