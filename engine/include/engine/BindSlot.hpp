#pragma once
#include <cstdint>
#include <windows.h>

namespace engine {
struct RenderingContext;

enum class ShaderStage : uint8_t {
    None = 0,
    Vertex = 1 << 0,
    Pixel = 1 << 1,
    Geometry = 1 << 2,
    Hull = 1 << 3,
    Domain = 1 << 4,
    Compute = 1 << 5,
};

DEFINE_ENUM_FLAG_OPERATORS(ShaderStage);

inline bool HasFlag(const ShaderStage slot, const ShaderStage flag) {
    return (slot & flag) != ShaderStage::None;
}


struct BindSlot {

    friend RenderingContext;

    [[nodiscard]] BindSlot WithStages(const ShaderStage stages) const {
        return BindSlot(id_, stages);
    }

    [[nodiscard]] bool HasStage(const ShaderStage stage) const {
        return HasFlag(stages_, stage);
    }

    constexpr explicit BindSlot(const uint8_t id, const ShaderStage stages) : id_(id), stages_(stages) {
    }

private :
    uint8_t id_{};
    ShaderStage stages_{};


};

struct BindSlots {
    constexpr static ShaderStage shaderStageAll = ShaderStage::Vertex | ShaderStage::Pixel | ShaderStage::Geometry |
        ShaderStage::Hull | ShaderStage::Domain | ShaderStage::Compute;

    struct CBuffer {
        constexpr static BindSlot Frame{0, shaderStageAll};
        constexpr static BindSlot View{1, shaderStageAll};
        constexpr static BindSlot Material{2, ShaderStage::Pixel};
        constexpr static BindSlot Object{3, shaderStageAll};
        //constexpr static constexpr static BindSlot ObjectNormal{4, shaderStageAll};
        constexpr static BindSlot ShadowCascades{4, shaderStageAll};

        constexpr static BindSlot Pass0{5, shaderStageAll}; //Light buffers for light
        constexpr static BindSlot Pass1{6, shaderStageAll};
        constexpr static BindSlot Pass2{7, shaderStageAll};
    };

    struct Texture {
        constexpr static BindSlot Albedo{0, ShaderStage::Pixel};
        constexpr static BindSlot Normal{1, ShaderStage::Pixel};
        constexpr static BindSlot Material{2, ShaderStage::Pixel};
        constexpr static BindSlot Depth{3, ShaderStage::Pixel};
        constexpr static BindSlot Shadow{4, ShaderStage::Pixel};
        constexpr static BindSlot SceneResult{5, ShaderStage::Pixel};
        
        constexpr static BindSlot Pass0{16, ShaderStage::Pixel};
        constexpr static BindSlot Pass1{17, ShaderStage::Pixel};
        constexpr static BindSlot Pass2{18, ShaderStage::Pixel};
    };

};
}
