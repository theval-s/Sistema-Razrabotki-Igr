#include <engine/rendering/ShaderParameters.hpp>
#include <engine/rendering/resources/ShaderContext.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <stdexcept>

namespace {
using namespace engine;

class ShaderParametersTest : public testing::TestWithParam<bool> {
protected:
    CompiledShader Compile(std::string_view source) {
        if (!GetParam())
            return ShaderContext::CompileSource(source, "shader_parameters_test.hlsl", ShaderType::Pixel);
        // Cooked representation: optimized DXBC with debug data removed, reflected
        // anew from the stored bytes, with no source/device dependency in Reflect.
        CompiledShader shader;
        ComPtr<ID3DBlob> compiled, errors;
        const auto hr = D3DCompile(source.data(), source.size(), "shader_parameters_test.hlsl",
                                  nullptr, nullptr, "PSMain", "ps_5_0",
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                  compiled.GetAddressOf(), errors.GetAddressOf());
        if (FAILED(hr))
            throw std::runtime_error(errors ? static_cast<const char*>(errors->GetBufferPointer())
                                            : "D3DCompile failed");
        if (FAILED(D3DStripShader(compiled->GetBufferPointer(), compiled->GetBufferSize(),
                                 D3DCOMPILER_STRIP_DEBUG_INFO, shader.blob.GetAddressOf())))
            throw std::runtime_error("D3DStripShader failed");
        if (FAILED(D3DReflect(shader.blob->GetBufferPointer(), shader.blob->GetBufferSize(),
                             IID_ID3D11ShaderReflection,
                             reinterpret_cast<void**>(shader.reflection.GetAddressOf()))))
            throw std::runtime_error("D3DReflect failed");
        return shader;
    }
};

void Set(ShaderParameters& parameters, std::initializer_list<ShaderParameterUpdate> updates) {
    parameters.Set(std::span<const ShaderParameterUpdate>(updates.begin(), updates.size()));
}

template<class T>
T Read(const ShaderParameters& parameters, size_t offset) {
    T value{};
    if (offset + sizeof(T) > parameters.Bytes().size()) {
        ADD_FAILURE() << "Read outside cbuffer at " << offset;
        return value;
    }
    std::memcpy(&value, parameters.Bytes().data() + offset, sizeof(T));
    return value;
}

std::vector<std::byte> CopyBytes(const ShaderParameters& parameters) {
    return {parameters.Bytes().begin(), parameters.Bytes().end()};
}

constexpr std::string_view scalarSource = R"(
cbuffer Parameters : register(b5) {
    float lead : packoffset(c0.x);
    float3 tint : packoffset(c0.y);
    float tail : packoffset(c1.x);
    float scalars[2] : packoffset(c2);
    float2 vectors[2] : packoffset(c4);
    int signedValue : packoffset(c6.x);
    uint unsignedValue : packoffset(c6.y);
    bool enabled : packoffset(c6.z);
    bool2 flags : packoffset(c7.x);
    float initial : packoffset(c7.z) = 2.5;
};
float4 PSMain(float4 position : SV_POSITION) : SV_TARGET {
    uint i = (uint)position.x & 1;
    return lead + float4(tint, tail) + scalars[i] + vectors[i].xyxy +
           signedValue + unsignedValue + (enabled ? 1 : 0) +
           (flags.x ? 2 : 0) + (flags.y ? 3 : 0) + initial;
}
)";

TEST_P(ShaderParametersTest, ScalarVectorArrayPackingAndDefaultsHaveDeterministicPadding) {
    auto shader = Compile(scalarSource);
    auto parameters = ShaderParameters::Reflect(shader, "Parameters");
    shader = {}; // The layout and initial bytes own everything they need.
    EXPECT_EQ(parameters.Slot(), 5u);
    ASSERT_EQ(parameters.Bytes().size(), 128u);
    ASSERT_EQ(parameters.Fields().size(), 10u);
    EXPECT_FLOAT_EQ(Read<float>(parameters, 120), 2.5f);
    for (size_t offset = 0; offset < 128; ++offset)
        if (offset < 120 || offset >= 124) EXPECT_EQ(parameters.Bytes()[offset], std::byte{0});
    Set(parameters, {
        {"lead", {1.0}}, {"tint", {2.0, 3.0, 4.0}}, {"tail", {5.0}},
        {"scalars", {6.0, 7.0}}, {"vectors", {8.0, 9.0, 10.0, 11.0}},
        {"signedValue", {-2147483648.0}}, {"unsignedValue", {4294967295.0}},
        {"enabled", {true}}, {"flags", {false, true}}
    });
    const std::array<size_t, 11> offsets{0, 4, 8, 12, 16, 32, 48, 64, 68, 80, 84};
    for (size_t i = 0; i < offsets.size(); ++i)
        EXPECT_FLOAT_EQ(Read<float>(parameters, offsets[i]), static_cast<float>(i + 1));
    EXPECT_EQ(Read<int32_t>(parameters, 96), std::numeric_limits<int32_t>::min());
    EXPECT_EQ(Read<uint32_t>(parameters, 100), std::numeric_limits<uint32_t>::max());
    EXPECT_EQ(Read<uint32_t>(parameters, 104), 1u);
    EXPECT_EQ(Read<uint32_t>(parameters, 112), 0u);
    EXPECT_EQ(Read<uint32_t>(parameters, 116), 1u);
    for (const auto offset : {20u, 24u, 28u, 36u, 40u, 44u, 52u, 56u, 60u, 72u, 76u, 88u, 92u, 108u, 124u})
        EXPECT_EQ(Read<uint32_t>(parameters, offset), 0u) << offset;
    Set(parameters, {{"lead", {-1.0}}});
    EXPECT_FLOAT_EQ(Read<float>(parameters, 0), -1.0f);
    EXPECT_FLOAT_EQ(Read<float>(parameters, 84), 11.0f);
    EXPECT_FLOAT_EQ(Read<float>(parameters, 120), 2.5f);
}

TEST_P(ShaderParametersTest, NonsquareMatricesAndMatrixArraysUseLogicalRowMajorInput) {
    auto parameters = ShaderParameters::Reflect(Compile(R"(
cbuffer Parameters : register(b9) {
    row_major float2x3 rm : packoffset(c0);
    column_major float2x3 cm : packoffset(c2);
    row_major float2x3 rms[2] : packoffset(c5);
    column_major float2x3 cms[2] : packoffset(c9);
};
float4 PSMain(float4 position : SV_POSITION) : SV_TARGET {
    uint i = (uint)position.x & 1;
    uint j = (uint)position.y % 3;
    return rm[i][j] + cm[i][j] + rms[i][i][j] + cms[i][i][j];
}
)"), "Parameters");
    EXPECT_EQ(parameters.Slot(), 9u);
    ASSERT_EQ(parameters.Bytes().size(), 240u);
    Set(parameters, {
        {"rm", {1.0, 2.0, 3.0, 4.0, 5.0, 6.0}},
        {"cm", {1.0, 2.0, 3.0, 4.0, 5.0, 6.0}},
        {"rms", {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0, 11.0, 12.0}},
        {"cms", {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0, 11.0, 12.0}}
    });
    // Independently specified GPU register images, including each padding word.
    const std::array<float, 60> expected{
        1,2,3,0, 4,5,6,0,
        1,4,0,0, 2,5,0,0, 3,6,0,0,
        1,2,3,0, 4,5,6,0, 7,8,9,0, 10,11,12,0,
        1,4,0,0, 2,5,0,0, 3,6,0,0,
        7,10,0,0, 8,11,0,0, 9,12,0,0
    };
    for (size_t word = 0; word < expected.size(); ++word)
        EXPECT_FLOAT_EQ(Read<float>(parameters, word * 4), expected[word]) << "word " << word;
    const auto& fields = parameters.Fields();
    ASSERT_EQ(fields.size(), 4u);
    EXPECT_TRUE(fields[0].rowMajor);
    EXPECT_FALSE(fields[1].rowMajor);
    EXPECT_EQ(fields[2].elementStride, 32u);
    EXPECT_EQ(fields[3].elementStride, 48u);
    EXPECT_EQ(fields[3].matrixStride, 16u);
}

TEST_P(ShaderParametersTest, InvalidUpdatesAreAtomicAndStrictlyTyped) {
    auto parameters = ShaderParameters::Reflect(Compile(scalarSource), "Parameters");
    Set(parameters, {{"lead", {42.0}}, {"enabled", {true}}});
    const auto before = CopyBytes(parameters);
    const std::vector<ShaderParameterUpdate> invalid{
        {"missing", {1.0}}, {"tint", {1.0, 2.0}}, {"tail", {}},
        {"enabled", {1.0}}, {"tail", {true}},
        {"signedValue", {1.5}}, {"signedValue", {2147483648.0}},
        {"signedValue", {-2147483649.0}}, {"unsignedValue", {-1.0}},
        {"unsignedValue", {4294967296.0}}, {"unsignedValue", {0.5}},
        {"tail", {std::numeric_limits<double>::infinity()}},
        {"tail", {std::numeric_limits<double>::quiet_NaN()}},
        {"tail", {std::numeric_limits<double>::max()}}
    };
    for (const auto& bad : invalid) {
        SCOPED_TRACE(bad.name);
        EXPECT_THROW(Set(parameters, {{"lead", {99.0}}, bad}), std::invalid_argument);
        EXPECT_EQ(CopyBytes(parameters), before);
    }
    EXPECT_THROW(Set(parameters, {{"lead", {1.0}}, {"lead", {2.0}}}), std::invalid_argument);
    EXPECT_EQ(CopyBytes(parameters), before);
    EXPECT_NO_THROW(parameters.Set({}));
    EXPECT_EQ(CopyBytes(parameters), before);
    Set(parameters, {{"signedValue", {2147483647.0}}, {"unsignedValue", {0.0}}, {"enabled", {false}}});
    EXPECT_EQ(Read<int32_t>(parameters, 96), std::numeric_limits<int32_t>::max());
    EXPECT_EQ(Read<uint32_t>(parameters, 100), 0u);
    EXPECT_EQ(Read<uint32_t>(parameters, 104), 0u);
}

TEST_P(ShaderParametersTest, UnsupportedTypesAreRejectedOnlyInTheRequestedBuffer) {
    const auto shader = Compile(R"(
struct Nested { float4 value; };
cbuffer Unsupported : register(b1) { Nested nested; };
cbuffer Parameters : register(b5) { float value; };
float4 PSMain() : SV_TARGET { return nested.value + value; }
)");
    EXPECT_NO_THROW(ShaderParameters::Reflect(shader, "Parameters"));
    try {
        (void)ShaderParameters::Reflect(shader, "Unsupported");
        FAIL() << "Struct field accepted";
    } catch (const std::invalid_argument& error) {
        EXPECT_NE(std::string(error.what()).find("nested"), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("struct"), std::string::npos);
    }
    EXPECT_THROW(ShaderParameters::Reflect(shader, "Missing"), std::invalid_argument);
    EXPECT_THROW(ShaderParameters::Reflect(shader, ""), std::invalid_argument);
    EXPECT_THROW(ShaderParameters::Reflect(CompiledShader{}, "Parameters"), std::invalid_argument);
    const auto doubles = Compile(R"(
cbuffer Parameters : register(b5) { double precision; };
float4 PSMain() : SV_TARGET { return (float)precision; }
)");
    EXPECT_THROW(ShaderParameters::Reflect(doubles, "Parameters"), std::invalid_argument);
}

TEST_P(ShaderParametersTest, AutomaticPackingAndSingleElementArraysFollowCompilerOffsets) {
    auto parameters = ShaderParameters::Reflect(Compile(R"(
cbuffer Parameters : register(b5) {
    float a;
    float3 b;
    float2 c;
    float3 d;
    float single[1];
    float last;
};
float4 PSMain() : SV_TARGET { return a + float4(b, c.x) + c.y + float4(d, single[0]) + last; }
)"), "Parameters");
    Set(parameters, {{"a", {1.0}}, {"b", {2.0, 3.0, 4.0}}, {"c", {5.0, 6.0}},
                     {"d", {7.0, 8.0, 9.0}}, {"single", {10.0}}, {"last", {11.0}}});
    EXPECT_FLOAT_EQ(Read<float>(parameters, 0), 1.0f);
    EXPECT_FLOAT_EQ(Read<float>(parameters, 4), 2.0f);
    EXPECT_FLOAT_EQ(Read<float>(parameters, 12), 4.0f);
    EXPECT_FLOAT_EQ(Read<float>(parameters, 16), 5.0f);
    EXPECT_FLOAT_EQ(Read<float>(parameters, 20), 6.0f);
    EXPECT_EQ(Read<uint32_t>(parameters, 24), 0u);
    EXPECT_FLOAT_EQ(Read<float>(parameters, 32), 7.0f);
    EXPECT_FLOAT_EQ(Read<float>(parameters, 40), 9.0f);
    EXPECT_FLOAT_EQ(Read<float>(parameters, 48), 10.0f);
    const auto& single = parameters.Fields()[4];
    EXPECT_TRUE(single.isArray);
    EXPECT_EQ(single.elements, 1u);
    EXPECT_EQ(single.elementStride, 16u);
    // The compiler's reflected offset is authoritative for tail-register reuse.
    EXPECT_FLOAT_EQ(Read<float>(parameters, parameters.Fields()[5].offset), 11.0f);
}

INSTANTIATE_TEST_SUITE_P(SourceAndCookedBytecode, ShaderParametersTest, testing::Bool());

} // namespace
