#include <engine/rendering/ShaderParameters.hpp>
#include <engine/rendering/resources/ShaderContext.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace engine {
namespace {

[[noreturn]] void Fail(std::string_view name, std::string_view reason) {
    throw std::invalid_argument("Shader parameter '" + std::string(name) + "': " + std::string(reason));
}

uint32_t ComponentOffset(const ShaderParameterField& field, uint32_t element,
                         uint32_t row, uint32_t column) {
    const auto within = field.matrixStride
        ? (field.rowMajor ? row * field.matrixStride + column * 4
                          : column * field.matrixStride + row * 4)
        : (row * field.columns + column) * 4;
    return element * field.elementStride + within;
}

void Encode(std::byte* destination, ShaderParameterType type,
            const ShaderParameterScalar& value, std::string_view name) {
    if (type == ShaderParameterType::Bool) {
        const auto* boolean = std::get_if<bool>(&value);
        if (!boolean) Fail(name, "bool components require boolean values, not numbers");
        const uint32_t encoded = *boolean ? 1u : 0u;
        std::memcpy(destination, &encoded, sizeof(encoded));
        return;
    }
    const auto* number = std::get_if<double>(&value);
    if (!number || !std::isfinite(*number)) Fail(name, "numeric components require finite numbers");
    switch (type) {
    case ShaderParameterType::Float: {
        if (std::abs(*number) > std::numeric_limits<float>::max())
            Fail(name, "number is outside the float32 range");
        const auto encoded = static_cast<float>(*number);
        std::memcpy(destination, &encoded, sizeof(encoded));
        break;
    }
    case ShaderParameterType::Int: {
        if (std::trunc(*number) != *number || *number < std::numeric_limits<int32_t>::min() ||
            *number > std::numeric_limits<int32_t>::max())
            Fail(name, "expected an exact integer in the int32 range");
        const auto encoded = static_cast<int32_t>(*number);
        std::memcpy(destination, &encoded, sizeof(encoded));
        break;
    }
    case ShaderParameterType::UInt: {
        if (std::trunc(*number) != *number || *number < 0 ||
            *number > std::numeric_limits<uint32_t>::max())
            Fail(name, "expected an exact integer in the uint32 range");
        const auto encoded = static_cast<uint32_t>(*number);
        std::memcpy(destination, &encoded, sizeof(encoded));
        break;
    }
    case ShaderParameterType::Bool: break;
    }
}

} // namespace

ShaderParameters ShaderParameters::Reflect(const CompiledShader& shader, std::string_view bufferName) {
    if (bufferName.empty() || bufferName.find('\0') != std::string_view::npos)
        Fail(bufferName, "expected a nonempty cbuffer name without embedded nulls");
    if (!shader.reflection) Fail(bufferName, "shader has no reflection");
    const std::string name(bufferName);
    auto* buffer = shader.reflection->GetConstantBufferByName(name.c_str());
    D3D11_SHADER_BUFFER_DESC bufferDesc{};
    if (!buffer || FAILED(buffer->GetDesc(&bufferDesc)))
        Fail(name, "cbuffer not found in shader reflection (it may have been optimized out)");
    if (bufferDesc.Type != D3D_CT_CBUFFER)
        Fail(name, "only constant buffers are supported");
    D3D11_SHADER_INPUT_BIND_DESC binding{};
    if (FAILED(shader.reflection->GetResourceBindingDescByName(name.c_str(), &binding)) ||
        binding.Type != D3D_SIT_CBUFFER || binding.BindCount != 1 ||
        binding.BindPoint >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT)
        Fail(name, "cbuffer must have one valid reflected constant-buffer binding");
    if (!bufferDesc.Size || bufferDesc.Size > 65536 || bufferDesc.Size % 16 != 0)
        Fail(name, "cbuffer size must be a nonzero multiple of 16 bytes, at most 64 KiB");

    ShaderParameters result;
    result.slot_ = binding.BindPoint;
    result.bytes_.resize(bufferDesc.Size, std::byte{0});
    std::unordered_set<std::string> names;
    for (UINT index = 0; index < bufferDesc.Variables; ++index) {
        auto* variable = buffer->GetVariableByIndex(index);
        D3D11_SHADER_VARIABLE_DESC desc{};
        if (!variable || FAILED(variable->GetDesc(&desc)) || !desc.Name)
            Fail(name, "could not reflect cbuffer variable");
        ShaderParameterField field;
        field.name = desc.Name;
        if (!names.insert(field.name).second) Fail(field.name, "duplicate reflected field");
        auto* reflectedType = variable->GetType();
        D3D11_SHADER_TYPE_DESC type{};
        if (!reflectedType || FAILED(reflectedType->GetDesc(&type)))
            Fail(field.name, "could not reflect variable type");
        if (type.Members || (type.Class != D3D_SVC_SCALAR && type.Class != D3D_SVC_VECTOR &&
                            type.Class != D3D_SVC_MATRIX_ROWS && type.Class != D3D_SVC_MATRIX_COLUMNS))
            Fail(field.name, "unsupported variable class; use scalar, vector or matrix fields instead of structs/objects");
        switch (type.Type) {
        case D3D_SVT_FLOAT: field.type = ShaderParameterType::Float; break;
        case D3D_SVT_INT: field.type = ShaderParameterType::Int; break;
        case D3D_SVT_UINT: field.type = ShaderParameterType::UInt; break;
        case D3D_SVT_BOOL: field.type = ShaderParameterType::Bool; break;
        default: Fail(field.name, "unsupported component type; only float32, int32, uint32 and bool are supported");
        }
        if (type.Rows < 1 || type.Rows > 4 || type.Columns < 1 || type.Columns > 4 ||
            (type.Class == D3D_SVC_SCALAR && (type.Rows != 1 || type.Columns != 1)) ||
            (type.Class == D3D_SVC_VECTOR && type.Rows != 1))
            Fail(field.name, "invalid reflected scalar/vector/matrix dimensions");
        field.rows = type.Rows;
        field.columns = type.Columns;
        field.isArray = type.Elements != 0;
        field.elements = field.isArray ? type.Elements : 1;
        field.offset = desc.StartOffset;
        field.size = desc.Size;
        field.rowMajor = type.Class == D3D_SVC_MATRIX_ROWS;
        const bool matrix = field.rowMajor || type.Class == D3D_SVC_MATRIX_COLUMNS;
        field.matrixStride = matrix ? 16u : 0u;
        // HLSL cbuffer ABI: each matrix major vector and each array element starts
        // at a 16-byte register. Reflection supplies shape, orientation and base offset.
        field.elementStride = matrix ? 16 * (field.rowMajor ? field.rows : field.columns)
                                     : field.isArray ? 16 : field.columns * 4;
        if (field.elements > 65536 / field.elementStride ||
            field.offset > bufferDesc.Size || field.size > bufferDesc.Size - field.offset ||
            field.offset % 4 != 0 || !field.size ||
            ((matrix || field.isArray) && field.offset % 16 != 0) ||
            (!matrix && !field.isArray && field.offset % 16 + field.columns * 4 > 16))
            Fail(field.name, "invalid reflected offset, size or register alignment");
        const auto lastEnd = ComponentOffset(field, field.elements - 1, field.rows - 1, field.columns - 1) + 4;
        if (lastEnd > field.size || field.size > field.elementStride * field.elements)
            Fail(field.name, "reflected variable size is inconsistent with its HLSL register layout");
        for (const auto& previous : result.fields_) {
            if (field.offset < previous.offset + previous.size && previous.offset < field.offset + field.size)
                Fail(field.name, "reflected variable overlaps another cbuffer field");
        }
        if (desc.DefaultValue) {
            // Copy components only, keeping register padding deterministically zero.
            for (uint32_t element = 0; element < field.elements; ++element)
                for (uint32_t row = 0; row < field.rows; ++row)
                    for (uint32_t column = 0; column < field.columns; ++column) {
                        const auto offset = ComponentOffset(field, element, row, column);
                        std::memcpy(result.bytes_.data() + field.offset + offset,
                                    static_cast<const std::byte*>(desc.DefaultValue) + offset, 4);
                    }
        }
        result.fields_.push_back(std::move(field));
    }
    return result;
}

void ShaderParameters::Set(std::span<const ShaderParameterUpdate> updates) {
    auto replacement = bytes_;
    std::unordered_set<std::string_view> names;
    for (const auto& update : updates) {
        if (!names.insert(update.name).second) Fail(update.name, "duplicate update");
        const auto found = std::find_if(fields_.begin(), fields_.end(),
                                      [&](const auto& field) { return field.name == update.name; });
        if (found == fields_.end()) Fail(update.name, "field not found in reflected cbuffer");
        const auto& field = *found;
        const auto count = field.elements * field.rows * field.columns;
        if (update.values.size() != count)
            Fail(update.name, "expected " + std::to_string(count) + " flattened component values");
        size_t valueIndex = 0;
        for (uint32_t element = 0; element < field.elements; ++element)
            for (uint32_t row = 0; row < field.rows; ++row)
                for (uint32_t column = 0; column < field.columns; ++column)
                    Encode(replacement.data() + field.offset + ComponentOffset(field, element, row, column),
                           field.type, update.values[valueIndex++], field.name);
    }
    bytes_.swap(replacement);
}

std::span<const std::byte> ShaderParameters::Bytes() const noexcept { return bytes_; }
const std::vector<ShaderParameterField>& ShaderParameters::Fields() const noexcept { return fields_; }
uint32_t ShaderParameters::Slot() const noexcept { return slot_; }

} // namespace engine
