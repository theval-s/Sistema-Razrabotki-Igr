#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <engine/engine_export.hpp>

namespace engine {

struct CompiledShader;

enum class ShaderParameterType { Float, Int, UInt, Bool };

struct ShaderParameterField {
    std::string name;
    ShaderParameterType type;
    uint32_t rows = 1;
    uint32_t columns = 1;
    // One for non-arrays; isArray distinguishes an HLSL array of length one.
    uint32_t elements = 1;
    uint32_t offset = 0;
    uint32_t size = 0; // Reflected variable size, possibly excluding final register padding.
    uint32_t elementStride = 0;
    uint32_t matrixStride = 0; // 16 for matrices, zero otherwise.
    bool rowMajor = false;
    bool isArray = false;
};

using ShaderParameterScalar = std::variant<double, bool>;

struct ShaderParameterUpdate {
    std::string name;
    // Consecutive array elements; each matrix is flattened in logical row-major order.
    std::vector<ShaderParameterScalar> values;
};

/// Device-independent owned layout and bytes for one reflected HLSL cbuffer.
/// Reflect and Set report invalid layouts/values with std::invalid_argument.
/// All padding starts at zero; reflected HLSL default component values are retained.
class ENGINE_API ShaderParameters {
public:
    static ShaderParameters Reflect(const CompiledShader& shader, std::string_view bufferName);
    /// Atomic partial update. Unknown/duplicate names and incorrect value counts fail.
    void Set(std::span<const ShaderParameterUpdate> updates);
    [[nodiscard]] std::span<const std::byte> Bytes() const noexcept;
    [[nodiscard]] const std::vector<ShaderParameterField>& Fields() const noexcept;
    [[nodiscard]] uint32_t Slot() const noexcept;

private:
    ShaderParameters() = default;
    std::vector<ShaderParameterField> fields_;
    std::vector<std::byte> bytes_;
    uint32_t slot_ = 0;
};

} // namespace engine
